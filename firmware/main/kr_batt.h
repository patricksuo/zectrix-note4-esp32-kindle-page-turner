/* Device battery: ADC1 CH3 (GPIO4, 1:2 divider) + CHRG_L/STDBY_H; the charge math lives in shared/battery_model.
 * Ticked synchronously in the app task. Screen refresh runs in another task, so check rail∧BUSY before sampling to stay clear of refreshes. */
#pragma once

#include <stdbool.h>

#include "battery_model.h"

void kr_batt_init(void);
/* Call from the app loop; ticks internally every 250 ms, samples every 60 s */
void kr_batt_tick(void);
battery_snap_t kr_batt_snap(void);
