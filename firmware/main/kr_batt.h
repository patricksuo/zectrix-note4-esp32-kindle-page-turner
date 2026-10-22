/* 本机电量：ADC1 CH3（GPIO4，1:2 分压）+ CHRG_L/STDBY_H，电量计算交给 shared/battery_model。
 * 在 app 任务里同步 tick。屏幕刷新在另一个任务里，采样前先看 rail∧BUSY，避开刷新期间。 */
#pragma once

#include <stdbool.h>

#include "battery_model.h"

void kr_batt_init(void);
/* app 循环里调；内部按 250 ms 节拍 tick、60 s 采一次 */
void kr_batt_tick(void);
battery_snap_t kr_batt_snap(void);
