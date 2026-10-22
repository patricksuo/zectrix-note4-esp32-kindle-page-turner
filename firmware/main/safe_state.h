/* Screen power rail switch, called by epd_session.c. Implemented in main.c (hold_dis → set → hold_en). */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t safe_state_set_epd_power(bool on);
