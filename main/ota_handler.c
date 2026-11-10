// main/ota_handler.c
//
// OTA updates over plain HTTP. The manifest served at {ota_url}/version.json
// describes the newest image:
//
//   {"version":"1.4.1","filename":"gateway-1.4.1.bin","sha256":"<64 hex>"}
//
// An update is installed only when the manifest version is strictly newer than
// the running app. Integrity is enforced with SHA-256 (there is no TLS). The
// target slot is the next OTA partition (ota_0 <-> ota_1); the bootloader
// rolls back if the new image does not confirm itself after boot.
#include "ota_handler.h"
#include "config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "cJSON.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static const char* TAG = "ota";

#define OTA_TASK_STACK              8192
#define OTA_TASK_PRIORITY           4
#define OTA_MANIFEST_MAX            1024
#define OTA_URL_MAX                 512
#define OTA_VERSION_MAX             32
#define OTA_FILENAME_MAX            128
#define OTA_SHA256_HEX_LEN          64
#define OTA_DL_BUF_SIZE             2048
#define OTA_CHECK_THROTTLE_US       (5LL * 1000 * 1000)
#define OTA_SLOT_COUNT              2

#define OTA_NOTIFY_CHECK            (1u << 0)
#define OTA_NOTIFY_UPDATE           (1u << 1)

typedef struct {
    char version[OTA_VERSION_MAX];
    char filename[OTA_FILENAME_MAX];
    char sha256[OTA_SHA256_HEX_LEN + 1];
} ota_manifest_t;

static TaskHandle_t s_task = NULL;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static char s_available_version[OTA_VERSION_MAX];
static char s_last_result[24] = "never";
static int64_t s_last_check_s = 0;
static bool s_busy = false;
static int64_t s_last_check_us = 0;

// --- small helpers -----------------------------------------------------------

static const char* ota_state_name(esp_ota_img_states_t state)
{
    switch (state) {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        default:                         return "unknown";
    }
}

static int64_t uptime_s(void)
{
    return esp_timer_get_time() / 1000000;
}

static void set_result(const char* result)
{
    portENTER_CRITICAL(&s_lock);
    snprintf(s_last_result, sizeof(s_last_result), "%s", result ? result : "");
    portEXIT_CRITICAL(&s_lock);
}

static void set_available_version(const char* version)
{
    portENTER_CRITICAL(&s_lock);
    if (version) {
        snprintf(s_available_version, sizeof(s_available_version), "%s", version);
    } else {
        s_available_version[0] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
}

static void set_busy(bool busy)
{
    portENTER_CRITICAL(&s_lock);
    s_busy = busy;
    portEXIT_CRITICAL(&s_lock);
}

static void mark_checked(const char* result)
{
    portENTER_CRITICAL(&s_lock);
    snprintf(s_last_result, sizeof(s_last_result), "%s", result ? result : "");
    s_last_check_s = uptime_s();
    portEXIT_CRITICAL(&s_lock);
}

static bool url_is_https(const char* url)
{
    return url && strncmp(url, "https://", 8) == 0;
}

// Joins a base URL and a relative name, tolerating trailing slashes in base.
static bool build_url(const char* base, const char* name, char* out, size_t out_len)
{
    if (!base || !base[0] || !name) {
        return false;
    }
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') {
        n--;
    }
    int written = snprintf(out, out_len, "%.*s/%s", (int)n, base, name);
    return written > 0 && (size_t)written < out_len;
}

// --- semver ------------------------------------------------------------------

// Parses exactly "major.minor.patch" (an optional leading 'v' is accepted).
static bool semver_parse(const char* s, int out[3])
{
    if (!s || !s[0]) {
        return false;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
        long value = 0;
        int digits = 0;
        while (isdigit((unsigned char)*s)) {
            value = value * 10 + (*s - '0');
            if (value > 999999) {
                return false;
            }
            s++;
            digits++;
        }
        if (digits == 0) {
            return false;
        }
        out[i] = (int)value;
        if (i < 2) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
    }
    return *s == '\0';
}

// Returns -1, 0 or 1; both inputs must be valid semver strings.
static int semver_cmp(const char* a, const char* b)
{
    int va[3] = { 0 };
    int vb[3] = { 0 };
    if (!semver_parse(a, va) || !semver_parse(b, vb)) {
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) {
            return va[i] < vb[i] ? -1 : 1;
        }
    }
    return 0;
}

// --- manifest ----------------------------------------------------------------

static bool manifest_filename_is_safe(const char* name)
{
    if (!name || !name[0] || strlen(name) >= OTA_FILENAME_MAX) {
        return false;
    }
    for (const char* p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
            return false;
        }
    }
    // Reject path traversal and hidden/dot-only names.
    if (strstr(name, "..") || name[0] == '.') {
        return false;
    }
    return true;
}

static bool manifest_sha_is_valid(const char* sha)
{
    if (!sha || strlen(sha) != OTA_SHA256_HEX_LEN) {
        return false;
    }
    for (const char* p = sha; *p; p++) {
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

// Fetches {ota_url}/version.json into *m. Returns ESP_OK on success.
static esp_err_t fetch_manifest(ota_manifest_t* m)
{
    const app_config_t* cfg = get_config();
    if (!cfg->ota_url || !cfg->ota_url[0]) {
        ESP_LOGE(TAG, "OTA URL is not configured");
        return ESP_ERR_INVALID_ARG;
    }

    char url[OTA_URL_MAX];
    if (!build_url(cfg->ota_url, "version.json", url, sizeof(url))) {
        ESP_LOGE(TAG, "OTA URL too long");
        return ESP_ERR_INVALID_ARG;
    }
    // Reject anything that is not an absolute http(s) URL, even if a stale NVS
    // value predates the config-level validation.
    if (!url_is_https(url) && strncmp(url, "http://", 7) != 0) {
        ESP_LOGE(TAG, "OTA URL must start with http:// or https://");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 256,
        // Never follow redirects: a redirect could downgrade an https URL to
        // plain http and bypass transport authentication.
        .disable_auto_redirect = true,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        // Verify the server certificate against the bundled CA store for
        // https:// URLs (ignored for plain http).
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Manifest request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "Manifest HTTP %d (%s)", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (content_len > OTA_MANIFEST_MAX) {
        ESP_LOGE(TAG, "Manifest too large: %lld bytes", (long long)content_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    char body[OTA_MANIFEST_MAX];
    int len = 0;
    while (len < (int)sizeof(body) - 1) {
        int r = esp_http_client_read(client, body + len, (int)sizeof(body) - 1 - len);
        if (r < 0) {
            ESP_LOGE(TAG, "Manifest read failed");
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        len += r;
    }
    bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (!complete || len == 0) {
        ESP_LOGE(TAG, "Incomplete manifest (%d bytes, complete=%d)", len, (int)complete);
        return ESP_ERR_INVALID_RESPONSE;
    }
    body[len] = '\0';

    cJSON* root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "Manifest is not valid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* jver = cJSON_GetObjectItem(root, "version");
    cJSON* jfile = cJSON_GetObjectItem(root, "filename");
    cJSON* jsha = cJSON_GetObjectItem(root, "sha256");
    bool fields_ok = cJSON_IsString(jver) && jver->valuestring &&
                     cJSON_IsString(jfile) && jfile->valuestring &&
                     cJSON_IsString(jsha) && jsha->valuestring;
    int parts[3];
    if (!fields_ok) {
        ESP_LOGE(TAG, "Manifest is missing version/filename/sha256");
        err = ESP_ERR_INVALID_RESPONSE;
    } else if (strlen(jver->valuestring) >= OTA_VERSION_MAX ||
               !semver_parse(jver->valuestring, parts)) {
        ESP_LOGE(TAG, "Manifest version is not semver: %s", jver->valuestring);
        err = ESP_ERR_INVALID_RESPONSE;
    } else if (!manifest_filename_is_safe(jfile->valuestring)) {
        ESP_LOGE(TAG, "Manifest filename is invalid: %s", jfile->valuestring);
        err = ESP_ERR_INVALID_RESPONSE;
    } else if (!manifest_sha_is_valid(jsha->valuestring)) {
        ESP_LOGE(TAG, "Manifest sha256 is invalid");
        err = ESP_ERR_INVALID_RESPONSE;
    } else {
        snprintf(m->version, sizeof(m->version), "%s", jver->valuestring);
        snprintf(m->filename, sizeof(m->filename), "%s", jfile->valuestring);
        for (int i = 0; i < OTA_SHA256_HEX_LEN; i++) {
            m->sha256[i] = (char)tolower((unsigned char)jsha->valuestring[i]);
        }
        m->sha256[OTA_SHA256_HEX_LEN] = '\0';
    }

    cJSON_Delete(root);
    return err;
}

// Fetches the manifest and compares it with the running app version.
// *newer is set when the manifest version is strictly greater.
static esp_err_t fetch_and_compare(ota_manifest_t* m, bool* newer)
{
    *newer = false;
    esp_err_t err = fetch_manifest(m);
    if (err != ESP_OK) {
        return err;
    }

    const esp_app_desc_t* desc = esp_app_get_description();
    const char* current = desc ? desc->version : "";
    int parts[3];
    if (!semver_parse(current, parts)) {
        ESP_LOGE(TAG, "Running app version is not semver: '%s'", current);
        return ESP_ERR_INVALID_VERSION;
    }
    int cmp = semver_cmp(m->version, current);
    ESP_LOGI(TAG, "Manifest version %s, running %s -> %s", m->version, current,
             cmp > 0 ? "update available" : "up to date");
    *newer = (cmp > 0);
    return ESP_OK;
}

// --- download + install ------------------------------------------------------

static void bytes_to_hex(const uint8_t* in, size_t len, char* out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

// Streams the image into the next OTA slot, verifies SHA-256 and selects the
// slot as the boot partition. On success this function restarts the device and
// never returns.
static esp_err_t download_and_apply(const ota_manifest_t* m)
{
    const app_config_t* cfg = get_config();
    char url[OTA_URL_MAX];
    if (!build_url(cfg->ota_url, m->filename, url, sizeof(url))) {
        ESP_LOGE(TAG, "Image URL too long");
        return ESP_ERR_INVALID_ARG;
    }
    if (!url_is_https(url)) {
        ESP_LOGW(TAG, "Image is fetched over plain HTTP; SHA-256 cannot stop a "
                      "man-in-the-middle from replacing it (use an https:// ota_url)");
    }

    const esp_partition_t* update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE(TAG, "No OTA update partition found");
        return ESP_ERR_NOT_FOUND;
    }
    // With the legacy factory + single ota_0 layout, a device booted from
    // ota_0 has no other OTA slot and esp_ota_begin() would fail only after
    // downloading the whole image. Fail early with a migration hint.
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && update->address == running->address) {
        ESP_LOGE(TAG, "Only one OTA slot and it is running; reflash the "
                      "partition table over USB (see README: ota_0 + ota_1)");
        return ESP_ERR_OTA_PARTITION_CONFLICT;
    }
    ESP_LOGI(TAG, "Installing %s into '%s' (%u bytes max)", m->filename,
             update->label, (unsigned)update->size);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = OTA_DL_BUF_SIZE,
        .buffer_size_tx = 256,
        .disable_auto_redirect = true,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "Image HTTP %d (%s)", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (content_len > 0 && (uint64_t)content_len > update->size) {
        ESP_LOGE(TAG, "Image (%lld B) does not fit slot (%u B)",
                 (long long)content_len, (unsigned)update->size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t* buf = malloc(OTA_DL_BUF_SIZE);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    psa_status_t hash_err = psa_hash_setup(&hash, PSA_ALG_SHA_256);
    if (hash_err != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_setup failed: %d", (int)hash_err);
        esp_ota_abort(ota);
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t total = 0;
    int64_t next_log = 128 * 1024;
    while (true) {
        int r = esp_http_client_read(client, (char*)buf, OTA_DL_BUF_SIZE);
        if (r < 0) {
            ESP_LOGE(TAG, "Image read failed after %u bytes", (unsigned)total);
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        if (psa_hash_update(&hash, buf, (size_t)r) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa_hash_update failed");
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, (size_t)r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            break;
        }
        total += (size_t)r;
        if ((int64_t)total >= next_log) {
            if (content_len > 0) {
                ESP_LOGI(TAG, "OTA progress: %u/%u bytes (%u%%)", (unsigned)total,
                         (unsigned)content_len, (unsigned)(total * 100 / content_len));
            } else {
                ESP_LOGI(TAG, "OTA progress: %u bytes", (unsigned)total);
            }
            next_log += 128 * 1024;
        }
    }

    bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);

    if (err == ESP_OK && (!complete || total == 0)) {
        ESP_LOGE(TAG, "Incomplete image (%u bytes, complete=%d)",
                 (unsigned)total, (int)complete);
        err = ESP_FAIL;
    }
    if (err == ESP_OK && content_len > 0 && (int64_t)total != content_len) {
        ESP_LOGE(TAG, "Image size mismatch: got %u, expected %lld",
                 (unsigned)total, (long long)content_len);
        err = ESP_ERR_INVALID_SIZE;
    }

    char digest_hex[OTA_SHA256_HEX_LEN + 1] = { 0 };
    if (err == ESP_OK) {
        uint8_t digest[32];
        size_t digest_len = 0;
        if (psa_hash_finish(&hash, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
            digest_len != sizeof(digest)) {
            ESP_LOGE(TAG, "psa_hash_finish failed");
            psa_hash_abort(&hash);
            err = ESP_FAIL;
        } else {
            bytes_to_hex(digest, digest_len, digest_hex);
            if (strcmp(digest_hex, m->sha256) != 0) {
                ESP_LOGE(TAG, "SHA-256 mismatch: got %s, expected %s",
                         digest_hex, m->sha256);
                err = ESP_ERR_INVALID_CRC;
            }
        }
    } else {
        psa_hash_abort(&hash);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA aborted, keeping the current firmware");
        esp_ota_abort(ota);
        return err;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA image verified (SHA-256 %s), booting '%s'", digest_hex,
             update->label);
    return ESP_OK;
}

// --- worker ------------------------------------------------------------------

static void ota_do_check(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_check_us != 0 && now - s_last_check_us < OTA_CHECK_THROTTLE_US) {
        ESP_LOGW(TAG, "Check throttled (less than 5 s since the previous one)");
        set_result("throttled");
        return;
    }
    s_last_check_us = now;
    set_busy(true);

    ota_manifest_t manifest;
    bool newer = false;
    esp_err_t err = fetch_and_compare(&manifest, &newer);
    if (err == ESP_OK) {
        set_available_version(newer ? manifest.version : NULL);
        mark_checked(newer ? "available" : "up_to_date");
    } else {
        set_available_version(NULL);
        mark_checked("error");
    }
    set_busy(false);
}

static void ota_do_update(void)
{
    set_busy(true);

    ota_manifest_t manifest;
    bool newer = false;
    esp_err_t err = fetch_and_compare(&manifest, &newer);
    if (err != ESP_OK) {
        set_available_version(NULL);
        mark_checked("error");
        set_busy(false);
        return;
    }
    if (!newer) {
        set_available_version(NULL);
        mark_checked("up_to_date");
        set_busy(false);
        return;
    }

    set_available_version(manifest.version);
    mark_checked("updating");
    err = download_and_apply(&manifest);
    if (err == ESP_OK) {
        // Give the HTTP status poll a moment to observe "updating", then boot
        // the freshly written slot.
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    set_available_version(manifest.version);
    mark_checked("error");
    set_busy(false);
}

static void ota_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "OTA worker started");

    while (true) {
        const app_config_t* cfg = get_config();
        TickType_t wait = portMAX_DELAY;
        if (cfg->ota_enable && cfg->ota_url && cfg->ota_url[0]) {
            uint64_t ms = (uint64_t)cfg->ota_check_interval_min * 60ULL * 1000ULL;
            if (ms > 0x7FFFFFFFULL) {
                ms = 0x7FFFFFFFULL;
            }
            wait = pdMS_TO_TICKS((uint32_t)ms);
        }

        uint32_t bits = 0;
        if (xTaskNotifyWait(0, 0xFFFFFFFFu, &bits, wait) == pdTRUE) {
            if (bits & OTA_NOTIFY_UPDATE) {
                ota_do_update();
            } else if (bits & OTA_NOTIFY_CHECK) {
                ota_do_check();
            }
        } else {
            // Periodic check (only possible when enabled + URL configured).
            ota_do_check();
        }
    }
}

// --- public API --------------------------------------------------------------

void init_ota(void)
{
    if (s_task) {
        return;
    }

    // The PSA subsystem provides SHA-256; auth_init() also calls this, and the
    // call is idempotent.
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_crypto_init failed: %d", (int)st);
    }

    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK, NULL, OTA_TASK_PRIORITY,
                    &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start OTA task");
        s_task = NULL;
        return;
    }

    const app_config_t* cfg = get_config();
    ESP_LOGI(TAG, "OTA ready (periodic checks %s, interval %u min)",
             cfg->ota_enable ? "enabled" : "disabled",
             (unsigned)cfg->ota_check_interval_min);
    if (cfg->ota_url && cfg->ota_url[0] && !url_is_https(cfg->ota_url)) {
        ESP_LOGW(TAG, "OTA URL uses plain HTTP; configure https:// so updates are "
                      "authenticated by TLS");
    }
}

void ota_trigger_check(void)
{
    if (!s_task) {
        ESP_LOGW(TAG, "OTA task is not running");
        return;
    }
    xTaskNotify(s_task, OTA_NOTIFY_CHECK, eSetBits);
}

void ota_trigger_update(void)
{
    if (!s_task) {
        ESP_LOGW(TAG, "OTA task is not running");
        return;
    }
    xTaskNotify(s_task, OTA_NOTIFY_UPDATE, eSetBits);
}

char* ota_get_status_json(void)
{
    const app_config_t* cfg = get_config();
    const esp_app_desc_t* desc = esp_app_get_description();

    char available[OTA_VERSION_MAX];
    char result[sizeof(s_last_result)];
    int64_t last_check = 0;
    bool busy = false;

    portENTER_CRITICAL(&s_lock);
    snprintf(available, sizeof(available), "%s", s_available_version);
    snprintf(result, sizeof(result), "%s", s_last_result);
    last_check = s_last_check_s;
    busy = s_busy;
    portEXIT_CRITICAL(&s_lock);

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "enabled", cfg->ota_enable != 0);
    cJSON_AddStringToObject(root, "url", cfg->ota_url ? cfg->ota_url : "");
    cJSON_AddNumberToObject(root, "check_interval_min", cfg->ota_check_interval_min);
    cJSON_AddStringToObject(root, "current_version", desc ? desc->version : "");
    if (available[0]) {
        cJSON_AddStringToObject(root, "available_version", available);
    } else {
        cJSON_AddNullToObject(root, "available_version");
    }
    cJSON_AddNumberToObject(root, "last_check_s", (double)last_check);
    cJSON_AddStringToObject(root, "last_result", result);
    cJSON_AddBoolToObject(root, "busy", busy);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char* ota_get_slots_json(void)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    // Add "running" field
    const char* running_label = running ? running->label : "";
    cJSON_AddStringToObject(root, "running", running_label);

    // Build "slots" array
    cJSON* slots = cJSON_CreateArray();
    if (!slots) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "slots", slots);

    for (int i = 0; i < OTA_SLOT_COUNT; i++) {
        const esp_partition_t* part = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP,
            (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + i),
            NULL);
        if (!part) {
            continue;
        }

        char version[OTA_VERSION_MAX] = "";
        const char* state_name = "inactive";
        bool desc_ok = false;

        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
            desc_ok = true;
            snprintf(version, sizeof(version), "%s", desc.version);
        }

        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(part, &state) == ESP_OK) {
            state_name = ota_state_name(state);
        }

        cJSON* slot = cJSON_CreateObject();
        if (!slot) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(slots, slot);

        cJSON_AddStringToObject(slot, "label", part->label);
        cJSON_AddNumberToObject(slot, "address", (double)part->address);
        cJSON_AddNumberToObject(slot, "size", (double)part->size);
        cJSON_AddStringToObject(slot, "version", version);
        cJSON_AddStringToObject(slot, "state", state_name);
        cJSON_AddBoolToObject(slot, "running", running ? (part->address == running->address) : false);
        cJSON_AddBoolToObject(slot, "bootable", desc_ok);
    }

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

esp_err_t ota_set_boot_slot(const char* label)
{
    if (!label) {
        return ESP_ERR_INVALID_ARG;
    }

    // Accept only "ota_0" and "ota_1"
    if (strcmp(label, "ota_0") != 0 && strcmp(label, "ota_1") != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_busy) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_lock);

    // Find the target partition
    const esp_partition_t* target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + (label[4] - '0')),
        NULL);
    if (!target) {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && target->address == running->address) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_app_desc_t desc;
    esp_err_t err = esp_ota_get_partition_description(target, &desc);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_set_boot_partition(target);
    if (err == ESP_OK) {
        set_busy(true);
        ESP_LOGI(TAG, "Boot slot switched to '%s' (%s)", target->label, desc.version);
    }
    return err;
}