# Known issues

证据标记：`[实测]` 本项目真机验证｜`[社区]` 第三方资料，未独立验证｜`[待测]` 尚未验证。

## Kindle 固件的行为（翻页器这边无法解决）

### 1. 翻页不会让 Kindle 的自动休眠重新计时

- **现象** `[实测 2026-10-22，Kindle Paperwhite 第 12 代，固件 5.20.1]`：最后一次触摸屏幕之后，
  即使一直用翻页器翻页，到了「设定 › 设备选项 › 休眠计时」设定的时间，Kindle 仍会先关掉前光，
  随后进入休眠（显示锁屏壁纸）。
- **亮度键也一样** `[实测]`：触摸一次屏幕后反复发送亮度 +/−，到时间仍然休眠。
- **原因** `[社区]`：
  - Kindle 处理翻页器按键的 `input_manager` 只在三个地方调用电源管理服务 powerd：电源键调用
    `setPowerButtonIfActiveAsync(1)`，亮度 +/− 调用 `stepBrightnessWithUI(±1)`。**没有任何地方**
    调用让休眠倒计时重新开始的 `setTouchSsTimeoutAsync`（出处：nes-dpad FINDINGS.md 2026-10-08
    的二进制分析；zuyan9 PROTOCOL.md 第 5 节结论相同）。
  - 翻页键被转发到一个叫 `kindle-input-pt` 的虚拟输入设备，powerd 不把它当作用户操作。
  - powerd 本身有重新计时的接口：越狱工具 kindle-button-mapper-rs 的「保持唤醒」功能就是每分钟执行
    `lipc-set-prop -i com.lab126.powerd touchScreenSaverTimeout 1`（`src/main.rs:32-33`）；
    另一份 Paperwhite 第 12 代（固件 5.18.5）的越狱教程是在每次翻页时重新设置 `screenSaverTimeout`。
    这两种做法都要在 Kindle 上运行自己的程序，未越狱的 Kindle 做不到。
  - 固件 5.18.5 的越狱教程已经写到「翻页器按键不算用户操作」，说明这是 Kindle 一直以来的行为，
    不是 5.20 才出现。
- **不是某一个翻页器实现的问题**：本项目按公开协议逐项实现，同样出现这个现象；目前已知的未越狱方案
  （zuyan9 的 nRF52840 和 Android 版、nes-dpad）都把它列为已知限制。
- **关于「Kindle 能认出官方翻页器，对第三方故意留了这个问题」**：没有找到任何证据。Kindle 只靠
  VID/PID 识别翻页器（广播包和 PnP ID 两处），配对是普通的 Just Works，没有任何加密认证；
  而且问题在于根本没有那一次调用，并不存在「按设备区别对待」的分支，所以按代码推断，官方翻页器
  在 5.20.1 上也会遇到同样的问题 `[推断]`。官方 Kindle Click 将于 2026-10-28 发售，届时随附的
  固件是否会补上这次调用 `[待测]`。
- **目前的应对**：建议用户把 Kindle 的休眠计时调长（zuyan9 README 也这样建议）；根本解决要等
  Amazon 更新固件。
- **后续验证办法**：拿到官方翻页器后，在同一固件版本上对比。如果官方翻页器没有这个问题，就用
  nRF Connect 读取它的广播内容、GATT 结构、PnP 版本号、固件版本字符串和 MAC 地址前缀，逐项改到
  本固件里测试，找出是哪一项造成了差别。另外，在 Kindle 搜索栏输入 `;dumpMessages` 能否在 5.20 上
  导出系统日志 `[待测]`。

### 2. 翻页器能让 Kindle 休眠，但不能唤醒它

- `[实测]` 休眠键有效；Kindle 休眠后再按（即使蓝牙还连着）没有任何反应。
- `[社区]` 休眠键调用的是 `setPowerButtonIfActiveAsync(1)`，只在 Kindle 处于使用状态时生效；
  唤醒接口 `setWakeUpAsync` 从未被调用；Kindle 只接受 5 个键，唤醒键 `KEY_WAKEUP` 会被丢弃。
  Kindle 休眠约 100 秒后会关闭蓝牙。官方 Kindle Click 的说明也只提到「同时长按两侧按键可让
  Kindle 休眠」。
- 实际使用：先按 Kindle 的电源键唤醒，约 2–4 秒后翻页器自动重连，然后就能翻页。Kindle 未就绪时
  按下的键直接丢弃，不会补发。

### 3. 按住翻页键只翻一页

`[社区]` Kindle 的设计（输入驱动会丢弃按键重复事件）。亮度键按住则会连续调节。

### 4. Kindle 会缓存翻页器的 GATT 结构

修改过 GATT 服务或 HID 报告描述符后，如果行为异常：先在 Kindle 上「忘记」该设备，再用串口 `c`
清除翻页器的配对记录，然后重新配对。

## 本固件与工具

1. **没有闲置自动关机**：不手动关机的话会一直以 20 ms 间隔广播，持续耗电。
2. **控制脚本开着时无法烧录**：串口被脚本占用，esptool 连不上。`tools/flash.sh` 会先检查，占用时直接拒绝。
3. **在 macOS 上打开 S3 的 USB 串口可能让芯片重启** `[实测]`：pyserial 先设 DTR 再设 RTS，中间会短暂
   出现 DTR=0 / RTS=1，这正是复位信号。`tools/kindle_remote.py` 已改为打开时两者保持为高，再先降 RTS、
   后降 DTR。
4. **在没有终端（tty）的环境里运行**：`flash.sh` 需要用 `echo yes |` 代替手动确认；控制脚本请用非交互模式。
5. **tsuuraifluens/kindle-curie 的文档声称「单键熄屏/唤醒」实测通过** `[社区]`：它的固件只发送
   Keyboard `0x66`（`KEY_POWER`），没有任何唤醒 Kindle 的实现；同一仓库的逆向分析
   （`docs/reverse_engineering/input_mapping.md`）也得出「`0x66` 无法唤醒屏保状态的 Kindle」的结论。
   我们认为那次实测只验证了休眠。
