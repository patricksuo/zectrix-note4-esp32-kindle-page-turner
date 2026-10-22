/* 关机 = 深度睡眠；按确认键唤醒 = 复位后重新执行 app_main。
 *
 * ⛔ 深睡前 GPIO17（主电源锁存）必须用 RTC hold 保持高电平，否则整机断电。
 * 交接步骤的顺序由 shared/safe_state_sleep_model 决定，本模块只负责调用对应的 API。
 *
 * 在 app 任务里调用（任务栈在内部 RAM；本固件不启用 PSRAM）。成功不返回；
 * 中途任何一步无法确认 ⇒ 在 PA 已为低的前提下 esp_restart()（GPIO17 的数字 hold
 * 在软复位期间保持高）；两种方式都无法确认 GPIO17 为高 ⇒ 原地停住，不重启也不睡。 */
#pragma once

void kr_power_off(void);
