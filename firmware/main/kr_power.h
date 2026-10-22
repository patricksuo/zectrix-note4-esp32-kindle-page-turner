/* Power off = deep sleep; waking with the confirm key = reset and run app_main again.
 *
 * ⛔ Before deep sleep, GPIO17 (main power latch) must be held high with RTC hold, or the whole device loses power.
 * The handoff step order is defined by shared/safe_state_sleep_model; this module only calls the matching APIs.
 *
 * Call from the app task (its stack is in internal RAM; this firmware doesn't enable PSRAM). Does not return on success;
 * if any step can't be confirmed ⇒ esp_restart() with PA already low (GPIO17's digital hold
 * stays high through a soft reset); if GPIO17 high can't be confirmed either way ⇒ park in place, no restart and no sleep. */
#pragma once

void kr_power_off(void);
