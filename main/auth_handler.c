// main/auth_handler.c
#include "auth_handler.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char* TAG = "auth";

#define AUTH_NS           "auth"
#define AUTH_USER_KEY     "user"
#define AUTH_PASS_KEY     "pass"
#define DEFAULT_USER      "admin"

// There is deliberately no fixed default password: on first boot a random one
// is generated and logged once, so a freshly flashed device cannot be taken
// over with well-known credentials.
#define AUTH_RANDOM_PASS_BYTES  12       // 12 random bytes -> 24 hex chars
#define AUTH_RANDOM_PASS_BUF    (AUTH_RANDOM_PASS_BYTES * 2 + 1)

#define SESSION_COOKIE_NAME  "session"
#define SESSION_ID_HEX_LEN   32          // 16 random bytes -> 32 hex chars
#define SESSION_ID_BUF       (SESSION_ID_HEX_LEN + 1)
#define SESSION_USER_BUF     33          // 32 chars + NUL
#define SESSION_COOKIE_MAX   256
#define SESSION_TTL_S        (24 * 60 * 60)

// Brute-force protection: after AUTH_MAX_FAILS consecutive bad logins the
// endpoint is locked for AUTH_LOCKOUT_S seconds for every client.
#define AUTH_MAX_FAILS       5
#define AUTH_LOCKOUT_S       60

#define PASS_HASH_LEN        64          // SHA-256 hex length

typedef struct {
    bool active;
    char id[SESSION_ID_BUF];
    char user[SESSION_USER_BUF];
    uint32_t created;
} auth_session_t;

static auth_session_t s_sessions[AUTH_MAX_SESSIONS];
static SemaphoreHandle_t s_session_mutex = NULL;

static char s_user[SESSION_USER_BUF] = DEFAULT_USER;
static char s_pass_hash[PASS_HASH_LEN + 1] = { 0 };

// Login throttle state, guarded by s_session_mutex.
static uint32_t s_login_fails = 0;
static uint32_t s_login_locked_until = 0;

// --- helpers -----------------------------------------------------------------

static esp_err_t reply_json(httpd_req_t* req, const char* status_line, const char* json)
{
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// Builds {"authenticated":bool[,"username":..][,"error":..]} via cJSON so the
// username can never break out of the JSON string.
static esp_err_t reply_auth(httpd_req_t* req, const char* status_line, bool authenticated,
                            const char* username, const char* error)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return reply_json(req, "500 Internal Server Error", "{\"error\":\"Out of memory\"}");
    }
    cJSON_AddBoolToObject(root, "authenticated", authenticated);
    if (username) {
        cJSON_AddStringToObject(root, "username", username);
    }
    if (error) {
        cJSON_AddStringToObject(root, "error", error);
    }
    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        return reply_json(req, "500 Internal Server Error", "{\"error\":\"Out of memory\"}");
    }
    esp_err_t err = reply_json(req, status_line, out);
    free(out);
    return err;
}

// Reads at most max_len bytes of the request body into buf (NUL-terminated).
// Returns the number of bytes read, -2 when the body is larger than buf (the
// unread part is purged by httpd after the handler returns), or -1 on error.
static int read_body(httpd_req_t* req, char* buf, size_t max_len)
{
    int total = req->content_len;
    if (total <= 0) {
        buf[0] = '\0';
        return 0;
    }
    if ((size_t)total > max_len - 1) {
        buf[0] = '\0';
        return -2;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        received += r;
    }
    buf[received] = '\0';
    return received;
}

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

// Renders `len` bytes as lowercase hex into `out` (2 * len + 1 bytes needed).
static void bytes_to_hex(const uint8_t* in, size_t len, char* out)
{
    static const char hexc[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hexc[in[i] >> 4];
        out[i * 2 + 1] = hexc[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

// SHA-256 of `in`, rendered as a 64-char lowercase hex string in `out`
// (out must hold at least PASS_HASH_LEN + 1 bytes).
static bool sha256_hex(const char* in, char* out)
{
    uint8_t digest[32];
    size_t digest_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)in, strlen(in),
                         digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
        digest_len != sizeof(digest)) {
        return false;
    }
    bytes_to_hex(digest, digest_len, out);
    return true;
}

// Fills `out` (AUTH_RANDOM_PASS_BUF bytes) with a random hex password.
static bool random_password(char* out, size_t out_len)
{
    if (!out || out_len < AUTH_RANDOM_PASS_BUF) {
        return false;
    }
    uint8_t raw[AUTH_RANDOM_PASS_BYTES];
    esp_fill_random(raw, sizeof(raw));
    bytes_to_hex(raw, sizeof(raw), out);
    return true;
}

// Length-independent string comparison (the lengths themselves are not secret
// for the fixed-size password hash).
static bool const_time_eq(const char* a, const char* b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t n = la < lb ? la : lb;
    uint8_t diff = (la == lb) ? 0 : 1;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)((uint8_t)a[i] ^ (uint8_t)b[i]);
    }
    return diff == 0;
}

// --- login throttle ----------------------------------------------------------

// Returns the remaining lockout in seconds (0 when not locked).
static uint32_t login_lock_remaining(void)
{
    if (!s_session_mutex) {
        return 0;
    }
    uint32_t now = uptime_s();
    uint32_t left = 0;
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    if (s_login_locked_until != 0 && (int32_t)(s_login_locked_until - now) > 0) {
        left = s_login_locked_until - now;
    }
    xSemaphoreGive(s_session_mutex);
    return left;
}

static void login_record_failure(void)
{
    if (!s_session_mutex) {
        return;
    }
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    if (s_login_fails < AUTH_MAX_FAILS) {
        s_login_fails++;
    }
    if (s_login_fails >= AUTH_MAX_FAILS) {
        s_login_locked_until = uptime_s() + AUTH_LOCKOUT_S;
        s_login_fails = 0;
        ESP_LOGW(TAG, "Too many failed logins; locked for %d s", AUTH_LOCKOUT_S);
    }
    xSemaphoreGive(s_session_mutex);
}

static void login_record_success(void)
{
    if (!s_session_mutex) {
        return;
    }
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    s_login_fails = 0;
    s_login_locked_until = 0;
    xSemaphoreGive(s_session_mutex);
}

// --- credentials -------------------------------------------------------------

static void load_or_create_creds(void)
{
    nvs_handle_t h;
    if (nvs_open(AUTH_NS, NVS_READWRITE, &h) != ESP_OK) {
        // Without NVS the credentials can only live in RAM. Use a fresh random
        // password (never a well-known default) and tell the operator.
        char pass[AUTH_RANDOM_PASS_BUF];
        strlcpy(s_user, DEFAULT_USER, sizeof(s_user));
        if (random_password(pass, sizeof(pass)) &&
            sha256_hex(pass, s_pass_hash)) {
            ESP_LOGE(TAG, "auth NVS unavailable; temporary credentials user '%s' password '%s'",
                     s_user, pass);
        } else {
            s_pass_hash[0] = '\0';
            ESP_LOGE(TAG, "auth NVS unavailable and password generation failed; "
                          "HTTP authentication is disabled until reboot");
        }
        return;
    }

    char user[SESSION_USER_BUF] = { 0 };
    char pass[PASS_HASH_LEN + 1] = { 0 };
    size_t len = sizeof(user);
    bool need_user = (nvs_get_str(h, AUTH_USER_KEY, user, &len) != ESP_OK || user[0] == '\0');
    len = sizeof(pass);
    bool need_pass = (nvs_get_str(h, AUTH_PASS_KEY, pass, &len) != ESP_OK ||
                      strlen(pass) != PASS_HASH_LEN);

    if (need_user) {
        strlcpy(user, DEFAULT_USER, sizeof(user));
        nvs_set_str(h, AUTH_USER_KEY, user);
    }
    if (need_pass) {
        char plain[AUTH_RANDOM_PASS_BUF];
        if (!random_password(plain, sizeof(plain)) || !sha256_hex(plain, pass)) {
            ESP_LOGE(TAG, "Failed to generate credentials; leaving auth unavailable");
        } else {
            nvs_set_str(h, AUTH_PASS_KEY, pass);
            ESP_LOGW(TAG, "Created credentials: user '%s', password '%s'", user, plain);
            ESP_LOGW(TAG, "Change the password via POST /auth/password");
        }
    }
    if (need_user || need_pass) {
        nvs_commit(h);
    }

    // Upgraded devices may still carry the old well-known password.
    if (!need_pass) {
        char legacy[PASS_HASH_LEN + 1];
        if (sha256_hex("admin", legacy) && const_time_eq(pass, legacy)) {
            ESP_LOGE(TAG, "Insecure default password is still active; "
                          "change it via POST /auth/password");
        }
    }

    strlcpy(s_user, user, sizeof(s_user));
    strlcpy(s_pass_hash, pass, sizeof(s_pass_hash));
    nvs_close(h);
}

static bool creds_save(const char* user, const char* pass_hash)
{
    nvs_handle_t h;
    if (nvs_open(AUTH_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open auth NVS for write");
        return false;
    }
    esp_err_t e1 = nvs_set_str(h, AUTH_USER_KEY, user);
    esp_err_t e2 = nvs_set_str(h, AUTH_PASS_KEY, pass_hash);
    esp_err_t e3 = nvs_commit(h);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK;
}

// --- sessions ----------------------------------------------------------------

static bool session_create(const char* user, char* id_out)
{
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    char id[SESSION_ID_BUF];
    bytes_to_hex(raw, sizeof(raw), id);

    bool ok = false;
    if (s_session_mutex) {
        uint32_t now = uptime_s();
        xSemaphoreTake(s_session_mutex, portMAX_DELAY);
        for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
            // Reclaim expired slots before allocating a new one.
            if (s_sessions[i].active && now - s_sessions[i].created >= SESSION_TTL_S) {
                s_sessions[i].active = false;
                s_sessions[i].id[0] = '\0';
                s_sessions[i].user[0] = '\0';
            }
            if (!s_sessions[i].active) {
                s_sessions[i].active = true;
                strlcpy(s_sessions[i].id, id, sizeof(s_sessions[i].id));
                strlcpy(s_sessions[i].user, user, sizeof(s_sessions[i].user));
                s_sessions[i].created = now;
                ok = true;
                break;
            }
        }
        xSemaphoreGive(s_session_mutex);
    }
    if (ok) {
        strlcpy(id_out, id, SESSION_ID_BUF);
    }
    return ok;
}

static bool session_lookup(const char* id, char* user_out, size_t user_len)
{
    if (!s_session_mutex) {
        return false;
    }
    bool found = false;
    uint32_t now = uptime_s();
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (!s_sessions[i].active) {
            continue;
        }
        if (now - s_sessions[i].created >= SESSION_TTL_S) {
            s_sessions[i].active = false;
            s_sessions[i].id[0] = '\0';
            s_sessions[i].user[0] = '\0';
            continue;
        }
        if (const_time_eq(s_sessions[i].id, id)) {
            if (user_out && user_len) {
                strlcpy(user_out, s_sessions[i].user, user_len);
            }
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_session_mutex);
    return found;
}

static void session_remove(const char* id)
{
    if (!s_session_mutex) {
        return;
    }
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (s_sessions[i].active && const_time_eq(s_sessions[i].id, id)) {
            s_sessions[i].active = false;
            s_sessions[i].id[0] = '\0';
            s_sessions[i].user[0] = '\0';
        }
    }
    xSemaphoreGive(s_session_mutex);
}

static void session_remove_all(void)
{
    if (!s_session_mutex) {
        return;
    }
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    memset(s_sessions, 0, sizeof(s_sessions));
    xSemaphoreGive(s_session_mutex);
}

// --- request parsing ---------------------------------------------------------

// Extracts the "session" cookie value from the Cookie header (if present).
static bool cookie_session_id(httpd_req_t* req, char* out, size_t out_len)
{
    size_t hlen = httpd_req_get_hdr_value_len(req, "Cookie");
    if (hlen == 0 || hlen >= SESSION_COOKIE_MAX) {
        return false;
    }
    char cookie[SESSION_COOKIE_MAX];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }

    const char* p = cookie;
    while (*p) {
        while (*p == ' ' || *p == ';') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char* eq = strchr(p, '=');
        const char* semi = strchr(p, ';');
        if (!eq || (semi && eq > semi)) {
            if (!semi) {
                break;
            }
            p = semi + 1;
            continue;
        }

        size_t name_len = (size_t)(eq - p);
        while (name_len > 0 && p[name_len - 1] == ' ') {
            name_len--;
        }
        const char* value = eq + 1;
        size_t value_len = semi ? (size_t)(semi - value) : strlen(value);
        while (value_len > 0 && value[value_len - 1] == ' ') {
            value_len--;
        }

        if (name_len == strlen(SESSION_COOKIE_NAME) &&
            strncmp(p, SESSION_COOKIE_NAME, name_len) == 0) {
            if (value_len == 0 || value_len >= out_len) {
                return false;
            }
            memcpy(out, value, value_len);
            out[value_len] = '\0';
            return true;
        }

        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    return false;
}

// True when the peer address is one of the device's own IP addresses: requests
// the firmware makes to itself (the on-boot HTTP self-test) are trusted.
static bool client_is_local(httpd_req_t* req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return false;
    }

    // The httpd listener may be an IPv6 (dual-stack) socket, in which case an
    // IPv4 peer arrives as an IPv4-mapped IPv6 address (::ffff:a.b.c.d).
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    if (getpeername(fd, (struct sockaddr*)&ss, &slen) != 0) {
        return false;
    }

    uint32_t peer = 0;
    if (ss.ss_family == AF_INET) {
        peer = ((struct sockaddr_in*)&ss)->sin_addr.s_addr;
    } else if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6* s6 = (const struct sockaddr_in6*)&ss;
        static const uint8_t v4_mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        if (memcmp(s6->sin6_addr.s6_addr, v4_mapped, sizeof(v4_mapped)) != 0) {
            return false; // genuine (non-mapped) IPv6 address
        }
        memcpy(&peer, s6->sin6_addr.s6_addr + 12, sizeof(peer));
    } else {
        return false;
    }
    if (peer == 0) {
        return false;
    }

    static const char* const keys[] = { "WIFI_AP_DEF", "WIFI_STA_DEF" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        esp_netif_t* n = esp_netif_get_handle_from_ifkey(keys[i]);
        if (!n) {
            continue;
        }
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(n, &info) != ESP_OK) {
            continue;
        }
        if (info.ip.addr != 0 && info.ip.addr == peer) {
            return true;
        }
    }
    return false;
}

// --- public API --------------------------------------------------------------

void auth_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    if (!s_session_mutex) {
        s_session_mutex = xSemaphoreCreateMutex();
        if (!s_session_mutex) {
            ESP_LOGE(TAG, "Failed to create session mutex");
        }
    }

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)st);
    }

    load_or_create_creds();
    ESP_LOGI(TAG, "Authentication ready (user '%s')", s_user);
}

bool auth_check(httpd_req_t* req)
{
    if (client_is_local(req)) {
        return true;
    }
    char id[SESSION_ID_BUF];
    if (!cookie_session_id(req, id, sizeof(id))) {
        return false;
    }
    return session_lookup(id, NULL, 0);
}

esp_err_t handle_auth_login(httpd_req_t* req)
{
    uint32_t lock_left = login_lock_remaining();
    if (lock_left > 0) {
        char retry[16];
        snprintf(retry, sizeof(retry), "%u", (unsigned)lock_left);
        httpd_resp_set_hdr(req, "Retry-After", retry);
        return reply_json(req, "429 Too Many Requests",
                          "{\"error\":\"Too many failed attempts, try again later\"}");
    }

    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Failed to read request body\"}");
    }

    cJSON* root = cJSON_Parse(body);
    if (!root) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Invalid JSON\"}");
    }
    cJSON* u = cJSON_GetObjectItem(root, "username");
    cJSON* p = cJSON_GetObjectItem(root, "password");
    bool have = cJSON_IsString(u) && u->valuestring && cJSON_IsString(p) && p->valuestring;

    bool ok = false;
    if (have) {
        char hash[PASS_HASH_LEN + 1];
        if (sha256_hex(p->valuestring, hash)) {
            // Evaluate both comparisons so a wrong username cannot be
            // distinguished from a wrong password by response timing.
            bool user_ok = const_time_eq(u->valuestring, s_user);
            bool pass_ok = const_time_eq(hash, s_pass_hash);
            ok = user_ok & pass_ok;
        }
    }
    cJSON_Delete(root);

    if (!have) {
        return reply_json(req, "400 Bad Request",
                          "{\"error\":\"Missing username or password\"}");
    }
    if (!ok) {
        login_record_failure();
        return reply_auth(req, "401 Unauthorized", false, NULL, "Invalid credentials");
    }
    login_record_success();

    char id[SESSION_ID_BUF];
    if (!session_create(s_user, id)) {
        return reply_json(req, "429 Too Many Requests",
                          "{\"error\":\"Too many active sessions\",\"max\":3}");
    }

    char cookie[160];
    // The device serves plain HTTP only, so the Secure attribute cannot be used
    // without making the session cookie unusable. SameSite=Strict plus the
    // HttpOnly flag block the practical CSRF/scripting vectors.
    snprintf(cookie, sizeof(cookie),
             "%s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d",
             SESSION_COOKIE_NAME, id, SESSION_TTL_S);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return reply_auth(req, "200 OK", true, s_user, NULL);
}

esp_err_t handle_auth_logout(httpd_req_t* req)
{
    char id[SESSION_ID_BUF];
    if (cookie_session_id(req, id, sizeof(id))) {
        session_remove(id);
    }
    httpd_resp_set_hdr(req, "Set-Cookie",
                       SESSION_COOKIE_NAME "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return reply_auth(req, "200 OK", false, NULL, NULL);
}

esp_err_t handle_auth_status(httpd_req_t* req)
{
    // Loopback (self-test) counts as authenticated.
    if (client_is_local(req)) {
        return reply_auth(req, "200 OK", true, s_user, NULL);
    }

    char id[SESSION_ID_BUF];
    char user[SESSION_USER_BUF];
    if (cookie_session_id(req, id, sizeof(id)) && session_lookup(id, user, sizeof(user))) {
        return reply_auth(req, "200 OK", true, user, NULL);
    }
    return reply_auth(req, "200 OK", false, NULL, NULL);
}

esp_err_t handle_auth_password(httpd_req_t* req)
{
    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Failed to read request body\"}");
    }

    cJSON* root = cJSON_Parse(body);
    if (!root) {
        return reply_json(req, "400 Bad Request", "{\"error\":\"Invalid JSON\"}");
    }

    cJSON* cur = cJSON_GetObjectItem(root, "current");
    cJSON* nw  = cJSON_GetObjectItem(root, "new");
    cJSON* un  = cJSON_GetObjectItem(root, "username");

    const char* current = (cJSON_IsString(cur) && cur->valuestring) ? cur->valuestring : NULL;
    const char* newpass = (cJSON_IsString(nw) && nw->valuestring) ? nw->valuestring : NULL;
    const char* newuser = (cJSON_IsString(un) && un->valuestring) ? un->valuestring : NULL;

    if (!current || current[0] == '\0') {
        cJSON_Delete(root);
        return reply_json(req, "400 Bad Request", "{\"error\":\"Current password required\"}");
    }
    if (!newpass || strlen(newpass) < 4) {
        cJSON_Delete(root);
        return reply_json(req, "400 Bad Request",
                          "{\"error\":\"Password must be at least 4 characters\"}");
    }

    char cur_hash[PASS_HASH_LEN + 1];
    if (!sha256_hex(current, cur_hash) || !const_time_eq(cur_hash, s_pass_hash)) {
        cJSON_Delete(root);
        return reply_json(req, "403 Forbidden", "{\"error\":\"Invalid current password\"}");
    }

    // Username is optional: an absent or empty value keeps the current name.
    char user[SESSION_USER_BUF];
    strlcpy(user, s_user, sizeof(user));
    if (newuser && newuser[0] != '\0') {
        if (strlen(newuser) >= sizeof(user)) {
            cJSON_Delete(root);
            return reply_json(req, "400 Bad Request", "{\"error\":\"Username too long\"}");
        }
        strlcpy(user, newuser, sizeof(user));
    }

    char new_hash[PASS_HASH_LEN + 1];
    if (!sha256_hex(newpass, new_hash)) {
        cJSON_Delete(root);
        return reply_json(req, "500 Internal Server Error",
                          "{\"error\":\"Failed to hash password\"}");
    }
    cJSON_Delete(root);

    if (!creds_save(user, new_hash)) {
        return reply_json(req, "500 Internal Server Error",
                          "{\"error\":\"Failed to save credentials\"}");
    }

    strlcpy(s_user, user, sizeof(s_user));
    strlcpy(s_pass_hash, new_hash, sizeof(s_pass_hash));
    session_remove_all();
    ESP_LOGW(TAG, "Credentials updated; all sessions invalidated");

    return reply_json(req, "200 OK", "{\"message\":\"Credentials updated\"}");
}