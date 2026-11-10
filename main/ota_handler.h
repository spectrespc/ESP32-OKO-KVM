// main/ota_handler.h
#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include "esp_err.h"

// Starts the background OTA worker task. Periodic manifest checks run only
// while ota_enable is set and ota_url is configured; manual checks and
// installs are always accepted through the trigger functions below.
void init_ota(void);

// Queues an immediate manifest check. Throttled to one check per 5 seconds.
void ota_trigger_check(void);

// Queues an immediate check-and-install. The worker downloads the image,
// verifies its SHA-256, selects the next boot partition and restarts.
void ota_trigger_update(void);

// Returns a malloc'ed JSON status object (caller frees) or NULL on OOM.
char* ota_get_status_json(void);

// Returns a malloc'ed JSON object describing OTA slots (caller frees) or NULL on OOM.
char* ota_get_slots_json(void);

// Sets the boot partition by label. Accepts "ota_0" and "ota_1".
// Returns ESP_OK on success or an error code otherwise.
esp_err_t ota_set_boot_slot(const char* label);

#endif // OTA_HANDLER_H