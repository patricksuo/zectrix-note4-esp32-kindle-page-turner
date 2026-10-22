/* 屏幕电源轨开关，供 epd_session.c 调用。实现在 main.c（hold_dis → set → hold_en）。 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t safe_state_set_epd_power(bool on);
