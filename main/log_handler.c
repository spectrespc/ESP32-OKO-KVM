// main/log_handler.c
#include "log_handler.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static const char* TAG = "log_handler";

#define LOG_RING_LINES 128
#define LOG_LINE_MAX   160

// Ring buffer of recent formatted log lines (ANSI escape sequences preserved
// so the web UI can colorize them).
static char s_lines[LOG_RING_LINES][LOG_LINE_MAX];
static int s_head = 0;   // next slot to write
static int s_count = 0;  // number of valid lines (<= LOG_RING_LINES)
static SemaphoreHandle_t s_mutex = NULL;

static vprintf_like_t s_prev_vprintf = NULL;

static void ring_push(const char* line)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    strlcpy(s_lines[s_head], line, LOG_LINE_MAX);
    s_head = (s_head + 1) % LOG_RING_LINES;
    if (s_count < LOG_RING_LINES) {
        s_count++;
    }

    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

// Replacement for the default log writer: format the line, store it, then
// forward the unmodified call to the original handler so the serial console
// keeps working. Never call ESP_LOG from here (it would recurse).
static int log_vprintf(const char* fmt, va_list args)
{
    char line[LOG_LINE_MAX];

    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n >= 0) {
        // Trim trailing newline(s) so the web UI can manage its own layout.
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        ring_push(line);
    }

    va_list fwd;
    va_copy(fwd, args);
    int ret = s_prev_vprintf ? s_prev_vprintf(fmt, fwd) : 0;
    va_end(fwd);
    return ret;
}

void init_log_handler(void)
{
    if (s_prev_vprintf) {
        ESP_LOGW(TAG, "Log handler already initialized");
        return;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create log ring buffer mutex");
        return;
    }

    s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "Log ring buffer active (%d lines)", LOG_RING_LINES);
}

esp_err_t handle_get_logs(httpd_req_t* req)
{
    // Snapshot the buffer under the lock first to avoid holding the lock
    // during serialization and allocation operations
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    int count = s_count;
    int start = (s_head - s_count + 2 * LOG_RING_LINES) % LOG_RING_LINES;
    
    // Pre-allocate arrays to avoid allocation inside the lock. Guard count==0:
    // malloc(0) may return NULL on newlib, which would spuriously 500 the
    // legitimate "no logs yet" case.
    char** lines_copy = malloc((count > 0 ? (size_t)count : 1) * sizeof(char*));
    if (!lines_copy) {
        if (s_mutex) {
            xSemaphoreGive(s_mutex);
        }
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", -1);
        return ESP_OK;
    }
    
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % LOG_RING_LINES;
        lines_copy[i] = strdup(s_lines[idx]);  // Duplicate string
        if (!lines_copy[i]) {
            // Clean up allocated strings on failure
            for (int j = 0; j < i; j++) {
                free(lines_copy[j]);
            }
            free(lines_copy);
            if (s_mutex) {
                xSemaphoreGive(s_mutex);
            }
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "{\"error\":\"Out of memory\"}", -1);
            return ESP_OK;
        }
    }
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }

    // Now build JSON outside the lock
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_CreateArray();
    if (!root || !arr) {
        // Clean up allocated strings
        for (int i = 0; i < count; i++) {
            free(lines_copy[i]);
        }
        free(lines_copy);
        cJSON_Delete(root);
        cJSON_Delete(arr);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", -1);
        return ESP_OK;
    }
    cJSON_AddItemToObject(root, "logs", arr);

    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(lines_copy[i]));
        free(lines_copy[i]);  // Free the duplicated string
    }
    free(lines_copy);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Serialization failed\"}", -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, strlen(out));
    free(out);
    return err;
}