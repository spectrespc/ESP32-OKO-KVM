#!/usr/bin/env python3
"""Generate a C fragment for the on-device HTTP self-test from a CSV.

The CSV is the single source of truth for the expected HTTP status codes,
shared with build_and_flash.sh. The generated header defines `s_tests`
using the `self_test_case_t` type declared immediately before its inclusion.

CSV format (lines starting with '#' and blank lines are ignored):
    METHOD,PATH,EXPECTED_STATUS
"""
import csv
import os
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gen_http_tests.py <input.csv> <output.h>", file=sys.stderr)
        return 2

    csv_path, out_path = sys.argv[1], sys.argv[2]

    rows = []
    with open(csv_path, newline="") as f:
        for row in csv.reader(f):
            if not row:
                continue
            first = row[0].strip()
            if not first or first.startswith("#"):
                continue
            if len(row) < 3:
                print(f"skipping malformed row: {row}", file=sys.stderr)
                continue
            method = first
            path = row[1].strip()
            expected = row[2].strip()
            rows.append((method, path, expected))

    lines = [
        f"/* Auto-generated from {os.path.basename(csv_path)} - do not edit. */",
        "static const self_test_case_t s_tests[] = {",
    ]
    for method, path, expected in rows:
        lines.append(f'    {{"{method}", "{path}", {expected}}},')
    lines.append("};")
    lines.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())