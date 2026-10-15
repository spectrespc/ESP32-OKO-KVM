// main/telnet_bridge.c
#include "telnet_bridge.h"
#include "config.h"
#include "sd_handler.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define TELNET_BUF_SIZE 256
#define TELNET_RETRY_MS 5000
#define TELNET_DISCONNECT_MS 500

// Per-bridge state. Two instances run independently, one per UART.
typedef struct {
    uart_port_t   uart_num;
    const char*   tag;          // "telnet_bridge0" / "telnet_bridge1"
    volatile bool running;
    volatile TaskHandle_t task;
    volatile int  listen_fd;
    volatile int  client_fd;
    // SD-card logging, set once at start from config (immutable afterwards).
    bool          sd_log_enable;
    const char*   sd_log_filename;  // "uar0.log" or "uar1.log"
} telnet_bridge_ctx_t;

static telnet_bridge_ctx_t s_bridge0 = {
    .uart_num = UART_NUM_0,
    .tag = "telnet_bridge0",
    .listen_fd = -1,
    .client_fd = -1,
    .sd_log_filename = "uar0.log",
};
static telnet_bridge_ctx_t s_bridge1 = {
    .uart_num = UART_NUM_1,
    .tag = "telnet_bridge1",
    .listen_fd = -1,
    .client_fd = -1,
    .sd_log_filename = "uar1.log",
};

// Append a chunk of relayed traffic to this bridge's SD log file. Nothing is
// logged on success; without a mounted card sd_write_rotate() is a silent
// no-op. Rotation keeps the file bounded at LOG_ROTATE_MAX_SIZE.
static void sd_log_write(telnet_bridge_ctx_t* ctx, const uint8_t* data, size_t len)
{
    if (!ctx->sd_log_enable || len == 0) {
        return;
    }
    sd_write_rotate(ctx->sd_log_filename, data, len);
}

// Address of the SoftAP interface. The bridge binds to this address so it is
// reachable only from the device's own AP, not any other interface.
static uint32_t ap_ip_addr(void)
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif == NULL) {
        return 0;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return 0;
    }
    return ip_info.ip.addr;
}

static void close_listen(telnet_bridge_ctx_t* ctx)
{
    // Invalidate the field before close() so a concurrent stop_bridge() cannot
    // close the same descriptor twice.
    int fd = ctx->listen_fd;
    ctx->listen_fd = -1;
    if (fd >= 0) {
        close(fd);
    }
}

static void close_client(telnet_bridge_ctx_t* ctx)
{
    int fd = ctx->client_fd;
    ctx->client_fd = -1;
    if (fd >= 0) {
        close(fd);
    }
}

static void relay_loop(telnet_bridge_ctx_t* ctx)
{
    uint8_t buf[TELNET_BUF_SIZE];

    while (ctx->running && ctx->client_fd >= 0) {
        // TCP client -> UART
        int n = recv(ctx->client_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            sd_log_write(ctx, buf, (size_t)n);
            uart_write_bytes(ctx->uart_num, (const char*)buf, n);
        } else if (n == 0) {
            break; // peer closed
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break; // socket error
        }

        // UART -> TCP client
        int r = uart_read_bytes(ctx->uart_num, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (r > 0) {
            sd_log_write(ctx, buf, (size_t)r);
            int off = 0;
            bool fatal = false;
            while (off < r) {
                int w = send(ctx->client_fd, buf + off, r - off, 0);
                if (w > 0) {
                    off += w;
                } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // Transient; SO_SNDTIMEO bounds each retry.
                    continue;
                } else {
                    // Fatal (EPIPE/ECONNRESET/EBADF): tear the connection down
                    // instead of silently dropping the remaining bytes.
                    fatal = true;
                    break;
                }
            }
            if (fatal) {
                break;
            }
        }
    }
}

static void telnet_bridge_task(void* arg)
{
    telnet_bridge_ctx_t* ctx = (telnet_bridge_ctx_t*)arg;

    // Resolve the listen port for this bridge's UART at task start.
    const app_config_t* cfg = get_config();
    uint16_t port = (ctx->uart_num == UART_NUM_0) ? cfg->telnet0_port : cfg->telnet1_port;
    if (port == 0) {
        port = (ctx->uart_num == UART_NUM_0) ? 2323 : 23;
    }

    while (ctx->running) {
        uint32_t ip = ap_ip_addr();
        if (ip == 0) {
            ESP_LOGW(ctx->tag, "SoftAP has no IP yet; retrying");
            vTaskDelay(pdMS_TO_TICKS(TELNET_RETRY_MS));
            continue;
        }

        ctx->listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ctx->listen_fd < 0) {
            ESP_LOGE(ctx->tag, "socket() failed: errno %d; retrying", errno);
            vTaskDelay(pdMS_TO_TICKS(TELNET_RETRY_MS));
            continue;
        }

        int opt = 1;
        setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = { 0 };
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ip;
        addr.sin_port = htons(port);

        if (bind(ctx->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            ESP_LOGE(ctx->tag, "bind() to port %d failed: errno %d; retrying", port, errno);
            close_listen(ctx);
            vTaskDelay(pdMS_TO_TICKS(TELNET_RETRY_MS));
            continue;
        }

        if (listen(ctx->listen_fd, 1) != 0) {
            ESP_LOGE(ctx->tag, "listen() failed: errno %d; retrying", errno);
            close_listen(ctx);
            vTaskDelay(pdMS_TO_TICKS(TELNET_RETRY_MS));
            continue;
        }

        // inet_ntoa_r writes into a caller buffer; the plain inet_ntoa shares a
        // single static buffer and is unsafe now that two tasks log concurrently.
        char ipbuf[16];
        ESP_LOGI(ctx->tag, "Telnet bridge listening on %s:%d (UART%d)",
                 inet_ntoa_r(addr.sin_addr, ipbuf, sizeof(ipbuf)), (int)port,
                 (int)ctx->uart_num);

        while (ctx->running) {
            struct sockaddr_in src;
            socklen_t src_len = sizeof(src);
            ctx->client_fd = accept(ctx->listen_fd, (struct sockaddr*)&src, &src_len);
            if (ctx->client_fd < 0) {
                if (!ctx->running) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // Bound blocking sends so a stalled client cannot wedge the relay.
            struct timeval snd_tmo = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(ctx->client_fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tmo, sizeof(snd_tmo));

            ESP_LOGI(ctx->tag, "Telnet client connected");
            relay_loop(ctx);
            close_client(ctx);
            ESP_LOGI(ctx->tag, "Telnet client disconnected");
            vTaskDelay(pdMS_TO_TICKS(TELNET_DISCONNECT_MS));
        }

        close_listen(ctx);
    }

    ctx->task = NULL;
    vTaskDelete(NULL);
}

static void start_bridge(telnet_bridge_ctx_t* ctx)
{
    if (ctx->running) {
        ESP_LOGW(ctx->tag, "Telnet bridge already running");
        return;
    }

    ctx->running = true;
    // xTaskCreate() wants a plain TaskHandle_t*, but ctx->task is volatile for
    // cross-task reads; create into a local and publish the handle afterwards.
    TaskHandle_t handle = NULL;
    if (xTaskCreate(telnet_bridge_task, ctx->tag, 4096, (void*)ctx, 5, &handle) != pdPASS) {
        ESP_LOGE(ctx->tag, "Failed to start telnet bridge task");
        ctx->running = false;
        return;
    }
    ctx->task = handle;
}

static void stop_bridge(telnet_bridge_ctx_t* ctx)
{
    if (!ctx->running) {
        return;
    }

    ctx->running = false;

    // Wake a blocked accept()/recv() so the task can notice the flag.
    if (ctx->listen_fd >= 0) {
        shutdown(ctx->listen_fd, SHUT_RDWR);
    }
    if (ctx->client_fd >= 0) {
        shutdown(ctx->client_fd, SHUT_RDWR);
    }

    // Allow the task to unwind and close its sockets.
    for (int i = 0; i < 50 && ctx->task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    close_client(ctx);
    close_listen(ctx);
    ESP_LOGI(ctx->tag, "Telnet bridge stopped");
}

void init_telnet_bridge0(void)
{
    const app_config_t* cfg = get_config();
    s_bridge0.sd_log_enable = cfg->uart0_sd_log != 0;
    start_bridge(&s_bridge0);
}

void init_telnet_bridge1(void)
{
    const app_config_t* cfg = get_config();
    s_bridge1.sd_log_enable = cfg->uart1_sd_log != 0;
    start_bridge(&s_bridge1);
}

void deinit_telnet_bridge0(void)
{
    stop_bridge(&s_bridge0);
}

void deinit_telnet_bridge1(void)
{
    stop_bridge(&s_bridge1);
}