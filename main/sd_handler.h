// main/sd_handler.h
#ifndef SD_HANDLER_H
#define SD_HANDLER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
    SD_STATUS_READY,
    SD_STATUS_BUSY,
    SD_STATUS_ERROR,
    SD_STATUS_UNKNOWN
} sd_status_t;

// Log-file rotation: when a file would grow past LOG_ROTATE_MAX_SIZE it is
// renamed to <name>.1 and older copies shift up, keeping LOG_ROTATE_DEPTH
// rotated copies before the oldest is deleted.
#define LOG_ROTATE_MAX_SIZE  (1024 * 1024)   // 1 MB
#define LOG_ROTATE_DEPTH     10              // keep up to 10 rotated copies

void init_sd_handler(void);
sd_status_t get_sd_status(void);
int sd_write_data(const char* filename, const uint8_t* data, size_t len);
// Same as sd_write_data but does not log a success line, so periodic or
// high-rate writers (sensor task, UART log flush) cannot flood the log ring.
int sd_write_data_quiet(const char* filename, const uint8_t* data, size_t len);
// Appends to /sdcard/<filename>, rotating the file once it would exceed
// LOG_ROTATE_MAX_SIZE (see above). Silent no-op (-1) when no card is mounted.
int sd_write_rotate(const char* filename, const uint8_t* data, size_t len);
int sd_read_data(const char* filename, uint8_t* buffer, size_t buffer_size);

// Directory listing / download helpers for the web file manager.
#define SD_LIST_OK               0
#define SD_LIST_ERR_NOT_MOUNTED  (-1)
#define SD_LIST_ERR_INVALID_PATH (-2)
#define SD_LIST_ERR_OPEN         (-3)
#define SD_LIST_ERR_NO_MEM       (-4)

// Returns true when rel is a safe path relative to the card root. Traversal
// (".."), backslashes and control bytes are rejected.
bool sd_rel_path_valid(const char* rel);
// Resolves rel against the mount point ("/sdcard", or "/sdcard/<rel>").
bool sd_resolve_path(const char* rel, char* out, size_t out_len);
// Lists one directory level as a JSON array of
// {"name","path","is_dir","size"} objects (caller frees *json_out).
int sd_list_dir(const char* rel, char** json_out);
// Opens a regular file for streaming downloads; returns NULL when missing or
// when no card is mounted. *size_out receives the file size.
FILE* sd_open_file(const char* rel, size_t* size_out);

int sd_list_files(char* buffer, size_t buffer_size);
void sd_cleanup(void);

#endif // SD_HANDLER_H