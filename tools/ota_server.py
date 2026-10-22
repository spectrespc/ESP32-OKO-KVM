#!/usr/bin/env python3
"""Minimal OTA manifest + firmware file server for the ESP32-C6 Gateway.

Serves a directory over plain HTTP and exposes:

    GET /version.json   -> {"version":"x.y.z","filename":"...bin","sha256":"..."}
    GET /<file>         -> static file (e.g. the firmware image)

The gateway compares versions and verifies the SHA-256 itself; this server only
publishes the manifest and the image. No HTTPS/TLS: run it on a trusted LAN.

Example:
    python3 tools/ota_server.py --root ./firmware \
        --version 1.4.1 --filename gateway-1.4.1.bin

Place the built image in ./firmware/gateway-1.4.1.bin and configure the device
with ota_url = http://<this-host>:8000 (Configuration -> Firmware update).
"""
import argparse
import hashlib
import json
import os
import re
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def find_single_image(root: str) -> str:
    images = sorted(
        name for name in os.listdir(root)
        if name.lower().endswith(".bin") and os.path.isfile(os.path.join(root, name))
    )
    if len(images) == 1:
        return images[0]
    if not images:
        raise SystemExit(f"error: no .bin file found in {root!r}; pass --filename")
    raise SystemExit(
        f"error: multiple .bin files in {root!r}; pass --filename "
        f"(found: {', '.join(images)})"
    )


class OtaHandler(SimpleHTTPRequestHandler):
    manifest_bytes = b""

    def do_GET(self):  # noqa: N802 (http.server naming)
        if urlsplit(self.path).path == "/version.json":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(self.manifest_bytes)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(self.manifest_bytes)
            return
        super().do_GET()

    def do_HEAD(self):  # noqa: N802
        if urlsplit(self.path).path == "/version.json":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(self.manifest_bytes)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        super().do_HEAD()

    def end_headers(self):
        # SimpleHTTPRequestHandler has no CORS support; not needed for the
        # device, but this keeps browser debugging simple.
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Serve an ESP32 OTA manifest (version.json) and firmware files over HTTP.",
        epilog=(
            "example:\n"
            "  python3 tools/ota_server.py --root ./firmware --version 1.4.1 "
            "--filename gateway-1.4.1.bin\n"
            "then set the device's ota_url to http://<this-host>:8000/"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--root", default="./firmware",
                        help="directory containing the .bin image (default: ./firmware)")
    parser.add_argument("--bind", default="0.0.0.0",
                        help="address to listen on (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8000,
                        help="TCP port (default: 8000)")
    parser.add_argument("--version", required=True,
                        help="manifest version, strictly newer than the running app (x.y.z)")
    parser.add_argument("--filename", default=None,
                        help="image file name inside --root (default: the only .bin found)")
    parser.add_argument("--sha256", default=None,
                        help="override the image SHA-256 (64 hex chars); computed from the file by default")
    return parser


def main() -> int:
    args = build_parser().parse_args()

    if not VERSION_RE.match(args.version):
        raise SystemExit(f"error: --version must be semver x.y.z (got {args.version!r})")

    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        raise SystemExit(f"error: --root is not a directory: {root}")

    filename = args.filename or find_single_image(root)
    if os.path.basename(filename) != filename or filename in (".", ".."):
        raise SystemExit("error: --filename must be a plain file name inside --root")
    image_path = os.path.join(root, filename)

    if args.sha256:
        sha = args.sha256.lower()
        if not SHA256_RE.match(sha):
            raise SystemExit("error: --sha256 must be 64 hex characters")
        if not os.path.isfile(image_path):
            print(f"warning: {image_path} does not exist; manifest will reference it anyway",
                  file=sys.stderr)
    else:
        if not os.path.isfile(image_path):
            raise SystemExit(f"error: image not found: {image_path}")
        sha = sha256_file(image_path)

    manifest = {
        "version": args.version,
        "filename": filename,
        "sha256": sha,
    }
    OtaHandler.manifest_bytes = (json.dumps(manifest, separators=(",", ":")) + "\n").encode()

    handler = partial(OtaHandler, directory=root)
    server = ThreadingHTTPServer((args.bind, args.port), handler)
    server.daemon_threads = True

    print(f"OTA root:   {root}")
    print(f"Image:      {filename} ({os.path.getsize(image_path) if os.path.isfile(image_path) else 'missing'} bytes)")
    print(f"Version:    {args.version}")
    print(f"SHA-256:    {sha}")
    print(f"Manifest:   http://{args.bind}:{args.port}/version.json")
    print(f"Device URL: http://<this-host>:{args.port}  (set ota_url on the gateway)")
    print("Press Ctrl+C to stop.")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())