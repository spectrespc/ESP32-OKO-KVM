// main/ext_control.h
#ifndef EXT_CONTROL_H
#define EXT_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

// Output level for a given polarity (pol: 0=active-low, 1=active-high). These
// are shared by the business logic (ext_control.c) and the low-level pin init
// (pins.c) so the active/inactive convention stays in one place.
static inline int ext_power_on_level(uint8_t pol)       { return pol ? 1 : 0; }
static inline int ext_power_off_level(uint8_t pol)      { return pol ? 0 : 1; }
static inline int ext_reset_active_level(uint8_t pol)   { return pol ? 1 : 0; }
static inline int ext_reset_inactive_level(uint8_t pol) { return pol ? 0 : 1; }

// Initializes the external power/reset GPIOs and restores the persisted power
// state. Idempotent: subsequent calls are a no-op. Must run after init_pins().
void init_ext_control(void);

// Turns the external device power on (true) or off (false): drives the GPIO and
// persists the new state to NVS. Returns the resulting state, or false when the
// power subsystem is disabled.
bool ext_power_set(bool on);

// Returns the current external power state (false when the subsystem is
// disabled or not yet initialized).
bool ext_power_get(void);

// Pulses the reset line for the configured duration (blocking for
// ext_rst_pulse_ms, max 5000 ms). Returns false when the reset subsystem is
// disabled or the request was throttled.
bool ext_reset_pulse(void);

// Returns a heap-allocated JSON status document for GET /ext/status. The caller
// owns the buffer and must free() it. Returns NULL on allocation failure.
char* ext_get_status_json(void);

#endif // EXT_CONTROL_H