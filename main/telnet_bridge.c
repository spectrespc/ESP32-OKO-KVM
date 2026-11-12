// main/telnet_bridge.c
// Minimal Telnet bridge for ESP-IDF. Implements:
//   - Welcome negotiation: sends WILL ECHO, WILL SGA on connect
//   - Client->UART: strips IAC sequences, raw bytes pass through
//   - UART->client: doubles 0xFF bytes (IAC) for Telnet protocol
// Raw TCP clients such as 'nc' will see the 6 welcome bytes and doubled 0xFF
// in binary traffic; this is intentional to remove double echo in 'telnet'.
#include "telnet_bridge.h"
#include "config.h"
#include "sd_handler.h"
#include "ntp_time.h"
#include "esp_log.h"
#include "esp_timer.h"
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

// SD log formatting. The worst case for text mode is 4x expansion ("\xNN"),
// so a 256-byte relay chunk is formatted and appended in <=128-byte slices to
// stay well inside the per-bridge buffer (which lives in .bss, not on the
// 4096-byte bridge task stack).
#define SD_LOG_SLICE_SIZE 128
#define SD_LOG_BUF_SIZE   1024
#define SD_LOG_HEX_COLS   16

// Telnet constants
#define TELNET_IAC 0xFF
#define TELNET_WILL 0xFB
#define TELNET_WONT 0xFC
#define TELNET_DO 0xFD
#define TELNET_DONT 0xFE
#define TELNET_SB 0xFA
#define TELNET_SE 0xF0
#define TELNET_NOP 0xF1
#define TELNET_DM 0xF2
#define TELNET_BRK 0xF3
#define TELNET_IP 0xF4
#define TELNET_EOR 0xEF
#define TELNET_GA 0xF7
#define TELNET_OPT_ECHO 0x01
#define TELNET_OPT_SGA 0x03

// Telnet parse state machine
typedef enum {
    TN_DATA = 0,
    TN_IAC,
    TN_OPT,
    TN_SB,
    TN_SB_IAC,
} telnet_parse_state_t;

// Per-bridge state. Two instances run independently, one per UART.
typedef struct {
    uart_port_t   uart_num;
    const char*   tag;          // "telnet_bridge0" / "telnet_bridge1"
    volatile bool running;
    volatile TaskHandle_t task;
    volatile int  listen_fd;
    volatile int  client_fd;
    // SD-card logging, set once at start from config (immutable afterwards).
    bool          sd_log_enable;    // uartN_sd_log at bridge start
    uint8_t       sd_log_fmt;       // UART_SD_LOG_FMT_* at bridge start
    const char*   sd_log_filename;  // "uar0.log" or "uar1.log"
    char          sd_log_buf[SD_LOG_BUF_SIZE];  // formatted chunk staging
    // Telnet parser state
    uint8_t tn_state;    // TN_DATA / TN_IAC / TN_OPT / TN_SB / TN_SB_IAC
    uint8_t tn_opt_cmd;  // WILL/WONT/DO/DONT awaiting its option byte
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

// Formats one slice of relayed traffic for the SD log. Text mode escapes
// everything outside printable ASCII plus CR/LF as "\xNN"; hexdump mode emits
// 16-byte lines with an ASCII column. Both prefix each output line with the
// local wall-clock time from ntp_stamp() ("[2026-11-12 11:15:55.123]") once
// SNTP has synced the clock, or with the uptime in milliseconds
// ("[<ms>ms]") before the first successful sync, plus the direction.
// Buffer sizing: a stamp prefix adds 25 bytes per line versus the uptime
// prefix, so text worst case is 25 + 4 + 4*128 = 541 bytes and a hexdump slice
// is 8 lines * 105 = 840 bytes; both stay inside SD_LOG_BUF_SIZE (1024).
// base_off is the slice offset within the relay chunk: hexdump uses it so the
// byte position keeps counting across slices, and text emits its prefix only
// for the first slice (one prefix per chunk).
// Returns the number of bytes written to dst (never including a NUL).
static size_t sd_log_format(char* dst, size_t dst_sz, uint8_t fmt, bool rx,
                            size_t base_off, const uint8_t* data, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    const char* dir = rx ? "RX" : "TX";
    uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    char stamp[32];
    bool have_stamp = ntp_stamp(stamp, sizeof(stamp));
    size_t out = 0;

    if (fmt == UART_SD_LOG_FMT_TEXT) {
        if (base_off == 0) {
            int n = have_stamp
                        ? snprintf(dst, dst_sz, "[%s] %s: ", stamp, dir)
                        : snprintf(dst, dst_sz, "[%llums] %s: ",
                                   (unsigned long long)uptime_ms, dir);
            if (n < 0 || (size_t)n >= dst_sz) {
                return 0;
            }
            out = (size_t)n;
        }

        for (size_t i = 0; i < len; i++) {
            uint8_t c = data[i];
            char esc[4];
            const char* piece;
            size_t piece_len;

            if ((c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r') {
                esc[0] = (char)c;
                piece = esc;
                piece_len = 1;
            } else {
                esc[0] = '\\';
                esc[1] = 'x';
                esc[2] = hex[c >> 4];
                esc[3] = hex[c & 0x0F];
                piece = esc;
                piece_len = 4;
            }
            if (out + piece_len + 1 > dst_sz) {
                break;
            }
            memcpy(dst + out, piece, piece_len);
            out += piece_len;
        }
        return out;
    }

    // Hexdump: one line per 16 input bytes.
    for (size_t off = 0; off < len; off += SD_LOG_HEX_COLS) {
        size_t line_len = len - off;
        if (line_len > SD_LOG_HEX_COLS) {
            line_len = SD_LOG_HEX_COLS;
        }
        int n = have_stamp
                    ? snprintf(dst + out, dst_sz - out, "[%s] %s %04x:  ",
                               stamp, dir, (unsigned)(base_off + off))
                    : snprintf(dst + out, dst_sz - out, "[%llums] %s %04x:  ",
                               (unsigned long long)uptime_ms, dir,
                               (unsigned)(base_off + off));
        if (n < 0 || out + (size_t)n >= dst_sz) {
            break;
        }
        out += (size_t)n;
        // Fixed-width hex column + " |" + ASCII column + "|\n".
        if (out + SD_LOG_HEX_COLS * 3 + SD_LOG_HEX_COLS + 5 > dst_sz) {
            break;
        }
        for (size_t i = 0; i < SD_LOG_HEX_COLS; i++) {
            if (i < line_len) {
                dst[out++] = hex[data[off + i] >> 4];
                dst[out++] = hex[data[off + i] & 0x0F];
                dst[out++] = ' ';
            } else {
                dst[out++] = ' ';
                dst[out++] = ' ';
                dst[out++] = ' ';
            }
        }
        dst[out++] = ' ';
        dst[out++] = '|';
        for (size_t i = 0; i < line_len; i++) {
            uint8_t c = data[off + i];
            dst[out++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
        }
        dst[out++] = '|';
        dst[out++] = '\n';
    }
    return out;
}

// Append a chunk of relayed traffic to this bridge's SD log file. Nothing is
// logged on success; without a mounted card sd_write_rotate() is a silent
// no-op. Rotation keeps the file bounded at LOG_ROTATE_MAX_SIZE.
static void sd_log_write(telnet_bridge_ctx_t* ctx, bool rx, const uint8_t* data, size_t len)
{
    if (!ctx->sd_log_enable || len == 0) {
        return;
    }

    if (ctx->sd_log_fmt != UART_SD_LOG_FMT_TEXT &&
        ctx->sd_log_fmt != UART_SD_LOG_FMT_HEXDUMP) {
        // Raw mode: byte-identical to the pre-formatter behaviour.
        sd_write_rotate(ctx->sd_log_filename, data, len);
        return;
    }

    for (size_t off = 0; off < len; off += SD_LOG_SLICE_SIZE) {
        size_t slice = len - off;
        if (slice > SD_LOG_SLICE_SIZE) {
            slice = SD_LOG_SLICE_SIZE;
        }
        size_t n = sd_log_format(ctx->sd_log_buf, sizeof(ctx->sd_log_buf),
                                 ctx->sd_log_fmt, rx, off, data + off, slice);
        if (n > 0) {
            sd_write_rotate(ctx->sd_log_filename, (const uint8_t*)ctx->sd_log_buf, n);
        }
    }
}

// Send all bytes from buffer, handling partial sends and timeouts.
// Returns true on success, false on fatal error.
static bool send_all(telnet_bridge_ctx_t* ctx, const uint8_t* data, int len);

// Filter client->UART data: strip Telnet IAC sequences, pass raw bytes through.
// Returns cleaned byte count (>=0), or -1 if negotiation reply failed.
static int telnet_filter_client(telnet_bridge_ctx_t* ctx, uint8_t* buf, int n)
{
    int out = 0;
    int i = 0;

    while (i < n) {
        uint8_t byte = buf[i];

        if (ctx->tn_state == TN_DATA) {
            if (byte == TELNET_IAC) {
                ctx->tn_state = TN_IAC;
            } else {
                buf[out++] = byte;
            }
            i++;
        } else if (ctx->tn_state == TN_IAC) {
            if (byte == TELNET_IAC) {
                // Escaped IAC: the client sent a literal 0xFF byte.
                buf[out++] = 0xFF;
                ctx->tn_state = TN_DATA;
            } else if (byte == TELNET_SB) {
                ctx->tn_state = TN_SB;
            } else if (byte == TELNET_WILL || byte == TELNET_WONT ||
                       byte == TELNET_DO || byte == TELNET_DONT) {
                ctx->tn_opt_cmd = byte;
                ctx->tn_state = TN_OPT;
            } else {
                // NOP, DM, BRK, IP, EOR, GA, SE, or unknown 2-byte command
                ctx->tn_state = TN_DATA;
            }
            i++;
        } else if (ctx->tn_state == TN_OPT) {
            // We have an option byte. Build response based on command type.
            uint8_t resp[3] = { TELNET_IAC, 0, buf[i] };
            uint8_t opt = buf[i];

            if (ctx->tn_opt_cmd == TELNET_DO) {
                // TELNET_DO -> WILL/WONT
                if (opt == TELNET_OPT_ECHO || opt == TELNET_OPT_SGA) {
                    resp[1] = TELNET_WILL;
                } else {
                    resp[1] = TELNET_WONT;
                }
                if (!send_all(ctx, resp, 3)) {
                    ctx->tn_state = TN_DATA;
                    return -1;
                }
            } else if (ctx->tn_opt_cmd == TELNET_WILL) {
                // TELNET_WILL -> DONT for unknown options
                if (opt == TELNET_OPT_ECHO || opt == TELNET_OPT_SGA) {
                    // Silent acceptance, no reply
                } else {
                    resp[1] = TELNET_DONT;
                    if (!send_all(ctx, resp, 3)) {
                        ctx->tn_state = TN_DATA;
                        return -1;
                    }
                }
            } else if (ctx->tn_opt_cmd == TELNET_WONT || ctx->tn_opt_cmd == TELNET_DONT) {
                // WONT/DONT -> swallowed silently
            }
            ctx->tn_state = TN_DATA;
            i++;
        } else if (ctx->tn_state == TN_SB) {
            if (byte == TELNET_IAC) {
                ctx->tn_state = TN_SB_IAC;
            }
            // Anything else is ignored in suboption
            i++;
        } else if (ctx->tn_state == TN_SB_IAC) {
            if (byte == TELNET_SE) {
                ctx->tn_state = TN_DATA;
            } else if (byte == TELNET_IAC) {
                // Nested IAC IAC in suboption - ignored
                ctx->tn_state = TN_SB;
            } else {
                ctx->tn_state = TN_SB;
            }
            i++;
        } else {
            ctx->tn_state = TN_DATA;
            i++;
        }
    }

    return out;
}

// Send data with 0xFF doubling for Telnet protocol (UART->client).
// Returns true on success, false on fatal error.
static bool send_stuffed(telnet_bridge_ctx_t* ctx, const uint8_t* data, int len)
{
    int off = 0;
    while (off < len) {
        const uint8_t* ff_pos = memchr(data + off, TELNET_IAC, len - off);
        int seg_len;

        if (ff_pos == NULL) {
            seg_len = len - off;
        } else {
            seg_len = ff_pos - (data + off);
        }

        // Send the segment [off, off + seg_len)
        if (seg_len > 0) {
            if (!send_all(ctx, data + off, seg_len)) {
                return false;
            }
        }

        if (ff_pos != NULL) {
            // Send the doubled IAC (0xFF 0xFF)
            uint8_t stuffed[2] = { 0xFF, 0xFF };
            if (!send_all(ctx, stuffed, 2)) {
                return false;
            }
            off = ff_pos - data + 1;
        } else {
            break;
        }
    }
    return true;
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

// Send all bytes from buffer, handling partial sends and timeouts.
// Returns true on success, false on fatal error.
static bool send_all(telnet_bridge_ctx_t* ctx, const uint8_t* data, int len)
{
    int off = 0;
    int stalls = 0;
    while (off < len) {
        int w = send(ctx->client_fd, data + off, len - off, 0);
        if (w > 0) {
            off += w;
            stalls = 0;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // SO_SNDTIMEO bounds each call; give up after a few stalled
            // timeouts so an unread socket cannot wedge the bridge forever.
            if (!ctx->running || ++stalls > 2) {
                return false;
            }
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static void relay_loop(telnet_bridge_ctx_t* ctx)
{
    uint8_t buf[TELNET_BUF_SIZE];

    while (ctx->running && ctx->client_fd >= 0) {
        // TCP client -> UART
        int n = recv(ctx->client_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            int cleaned = telnet_filter_client(ctx, buf, n);
            if (cleaned < 0) {
                break; // negotiation reply failed
            }
            if (cleaned > 0) {
                // Client -> UART traffic is logged after IAC stripping, exactly
                // as it is written to the device.
                sd_log_write(ctx, false, buf, (size_t)cleaned);
                uart_write_bytes(ctx->uart_num, (const char*)buf, cleaned);
            }
        } else if (n == 0) {
            break; // peer closed
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break; // socket error
        }

        // UART -> TCP client
        int r = uart_read_bytes(ctx->uart_num, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (r > 0) {
            sd_log_write(ctx, true, buf, (size_t)r);
            if (!send_stuffed(ctx, buf, r)) {
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
        // Listen on all interfaces: the bridge is reachable both from the
        // device's SoftAP and from the LAN via the STA address.
        addr.sin_addr.s_addr = INADDR_ANY;
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

        ESP_LOGI(ctx->tag, "Telnet bridge listening on 0.0.0.0:%d (UART%d)",
                 (int)port, (int)ctx->uart_num);

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

            // Send Telnet welcome negotiation: WILL ECHO, WILL SGA
            static const uint8_t welcome[] = {
                TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO,
                TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA
            };
            ctx->tn_state = TN_DATA;
            ctx->tn_opt_cmd = 0;
            if (!send_all(ctx, welcome, sizeof(welcome))) {
                ESP_LOGW(ctx->tag, "Failed to send Telnet welcome, closing client");
                close_client(ctx);
                vTaskDelay(pdMS_TO_TICKS(TELNET_DISCONNECT_MS));
                continue;
            }

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
    s_bridge0.sd_log_fmt = cfg->uart0_sd_log_fmt;
    start_bridge(&s_bridge0);
}

void init_telnet_bridge1(void)
{
    const app_config_t* cfg = get_config();
    s_bridge1.sd_log_enable = cfg->uart1_sd_log != 0;
    s_bridge1.sd_log_fmt = cfg->uart1_sd_log_fmt;
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