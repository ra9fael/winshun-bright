# ra9fael.bright 功能规格（v0.1.0）

状态：v0.1.0 已实现，并已在真实宿主实测（实测状态见第 12 节）。协议与清单字段以 WinShun 0.22.0 插件文档为准（resident JSON-RPC over stdio）。

## 1 目标

- 键盘+鼠标直达的亮度控制：`bright` 列表逐台，手势全局
- 内屏/外屏统一百分比体验
- 自带暖色滤镜（不碰系统夜灯）

## 2 非目标

- 不做 HDR 开关（DisplayConfig 路线后置）
- 不改系统夜灯（CloudStore 注册表未公开，跨版本易碎）
- 不做色温 K 值精确校准（gamma 近似，主观舒适优先）
- 不做定时调度

## 3 命令（`bright` / `亮度`）

- 留空：每台显示器一行——名称（GDI 设备名 + EDID 名称）、类型（内屏/外屏）、当前亮度 %、通道（WMI / DDC）；不支持的显示器标「不支持」
- `bright 70`：全部可调显示器设为 70（0–100，越界 clamp 并在结果行注明）
- 动作（作用于所选行对应的那台显示器）：`Enter` +10、`Ctrl+Enter` −10、`Shift+Enter` 100

## 4 手势（复用 volume 架构）

- **修饰键可配**：`gestureModifiers` 默认 `ctrl+alt`，可选 `alt+shift` / `win+alt` / `ctrl+shift+alt`。修饰键必须与配置**完全一致**；只 Ctrl（volume 手势）、多余的 Shift/Win、组合不全 → 一律透传
- **Ctrl+Alt+滚轮**：调**鼠标所在那块**显示器的亮度（`gestureTarget` 默认 `monitor-under-mouse`；选 `all` 则调所有可调显示器），浮层显示该屏新值与名称
  - SendInput 注入的滚轮 → 透传，防自触发循环
  - 指针落在不可调的屏上 → 吞掉事件并浮层说明（远程会话另给专门文案）
  - 指针落在两屏缝隙 → 取最近那块，不随机
- **Ctrl+Alt+左键**：暖色滤镜开/关（`warmStrength` > 0 时生效），浮层显示当前状态。gamma 是按 DC 的，仍是全部屏一起
- **快捷键**：`hotkeyUp` / `hotkeyDown` / `hotkeyMax` 三个可录制热键，等价于滚轮/100%，目标屏规则同上；宿主 `capabilities.hostHotkeys` 为 true 时经 `register_hotkeys` 代注册（按键以 `hotkey` 通知回来），否则插件自建 `RegisterHotKey` 线程
- 钩子健康：`WH_MOUSE_LL` 被系统摘掉后每 10 秒自愈重装；重装失败写日志

## 5 亮度通道

- 内屏（WMI）：
  - 连接 `root\wmi`：`WmiMonitorBrightness` / `WmiMonitorBrightnessMethods`（`WmiSetBrightness`）
  - `WmiMonitorID` 判定内屏
- 外屏（DDC/CI）：
  - `EnumDisplayMonitors` → `GetNumberOfPhysicalMonitorsFromHMONITOR` → `GetVCPFeatureAndVCPFeatureReply` / `SetVCPFeature`（VCP code 0x10）
  - VCP 最大值 ≠ 100 时按比例换算并缓存 max
- DDC 落定延迟（数十毫秒）：浮层先显示本地预测值，约 800ms 后回读；差异 >2% 时校准浮层
- 会话内缓存各显示器当前值；`WM_DISPLAYCHANGE` / 热插拔后重建列表与缓存

## 6 暖色滤镜

- `SetDeviceGammaRamp`：每台显示器 DC 建 3×256 LUT；`warmStrength` 从 6500K 线性降到约 3400K 的近似曲线（R 不变、G/B 衰减）
- 开启时保存原始 ramp；关闭、进程退出、会话切换（WTS 事件）时恢复
- LUT 值域 clamp 在合法 gamma ramp 范围内，避免驱动拒绝
- `warmRemember=true` 时状态与强度记入 `WS_DATA_DIR\state.json`，宿主启动时恢复

## 6.5 软件亮度模拟

硬件通道（WMI / DDC）都到不了的屏（电视接 AVR、投影、RDP 虚拟屏）用遮罩模拟变暗，不影响能调的屏。

- **遮罩窗口**：每屏一个 `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST` 顶层窗口，覆盖该屏 `rcMonitor`，`SetLayeredWindowAttributes(0, alpha, LWA_ALPHA)`；`WM_NCHITTEST` 返回 `HTTRANSPARENT` 双保险
- **alpha 映射**：`dimAlpha(percent, maxDark) = round(maxDark/100*255 * (100-percent)/100)`；100% → 0（不建窗口），0% → 上限；`maxDark` 夹在 0–90
- **设置**：`softwareBrightness`（`off` / `unsupported-only` / `always`）、`softwareMaxDark`（0–90，默认 60）
- **手势**：命中模拟屏时改遮罩 alpha 并记在 `dimState`，浮层显示该值；`always` 时所有屏叠加（硬件 + 软件双重变暗）
- **列表**：`bright` 行为模拟屏标「软件模拟」，`data.simulated` 为 true，不再标「不支持」
- **远程会话**：遮罩不依赖物理屏，RDP 内可用；列表说明改为引导开启该设置
- 生命周期：归 worker 线程；`WM_DISPLAYCHANGE` 重建；进程退出全部销毁，屏上无残留

## 6.6 蘑菇彩蛋

在同一显示器上连续向上滚动 4 秒触发，允许触摸板/滚轮事件之间最多约 10 秒间隔，不受时间和亮度值限制；即使手势目标设为全部显示器，也按鼠标所在屏跟踪。`mushroomEgg` 默认开启，可在插件设置中关闭。

- 动画绘制在**独立的全屏彩蛋窗口**（类名 `WinShunBrightEgg`），覆盖触发手势所在显示器，不覆盖其它屏幕
- 窗口使用逐像素透明 layered window；只绘制emoji/粒子，未绘制区域完全透出原桌面；不改变亮度、不染色、不抢焦点、不拦截鼠标
- 六种动效随机选一，每 1.8 秒随机切换下一种，循环持续播放，直到用户用鼠标左键点击停止
- emoji与粒子在整块显示器内散布，粒子轨迹覆盖全屏；glyph等比缩放，圆环保持正圆
- 鼠标点击不被特效窗吞掉；Ctrl+Alt+左键仍可同时停止彩蛋并切换暖色滤镜
- 与亮度提示窗口完全分离，各自持有窗口、定时器与渲染目标；继续滚动产生的百分比提示或 800ms 回读校准提示不会替换彩蛋
- 用户关闭开关时立即隐藏正在播放的动画

## 7 浮层

复用 volume 浮层实现：九宫格锚点 + 偏移、非激活、自动消失、DPI 感知。显示 `<显示器名或"全部"> NN%` + 进度条；暖色切换时显示开/关一行文案。

## 8 权限

- `ui`：低级鼠标钩子 + 浮层窗口
- `system:settings`：亮度属系统设置面（WMI/DDC/gamma 本身不需要提权）
- 不做 `elevated`、`network`、`clipboard`

## 9 数据位置

默认不持久化；`warmRemember` 时仅 `WS_DATA_DIR\state.json`（int 强度 + bool 开关）。

## 10 失败处理

- 远程桌面会话（mstsc/RDP）→ 列表与浮层说明"只暴露虚拟屏，到不了物理显示器"，不把物理屏标成「不支持」
- 无 WMI 亮度类 → 该行走 DDC 尝试；首次手势时再试一次（惰性重绑）；仍不支持则标「不支持」，并给出失败原因（no-handle / vcp-read / vcp-write / wmi-write）
- DDC 不响应（线材/坞站不支持）→ 标「不支持」，动作给出说明
- 全部不支持 → 留空查询返回提示行，手势吞掉并解释
- `SetVCPFeature` 失败 → 浮层/通知报错，不假装成功
- gamma ramp 被其他程序接管（f.lux 类）→ 后写者生效，不轮询抢夺
- 低级钩子被系统移除（超时）→ 每 10 秒自愈重装；连续失败写日志并停用手势

## 11 里程碑

- v0.1：上述全部（DDC + WMI + 滤镜 + 手势 + 浮层）
- v0.2：按鼠标所在屏幕独立调节、可配修饰键、可录制快捷键、远程会话识别、DDC 惰性重绑、钩子自愈
- v0.3 候选：HDR 开关、暖色滤镜按屏、系统夜灯状态只读检测

## 12 实现记录（v0.1.0 回填）

与上文规格的偏差，以及实施后的实测状态。

### 架构

- 单进程单文件 `bright.exe`（与 volume 同构），无 `BrightApp.dll`、不链接 Qt；`plugin.json` 因此删去 `qt` / `qtVersion`。浮层是 Win32 非激活窗口。
- 线程：协议线程（query/action，自己持有一个后端实例）、钩子线程（只做判定与投递）、工作线程（WMI/DDC/gamma + 浮层 + 800ms 回读定时器）。
- 热插拔：工作线程持有一个从不显示的顶层窗口接收 `WM_DISPLAYCHANGE`，重建列表与 DDC 句柄，并重新套用暖色 ramp。
- 纯逻辑在 `src/bright_logic.{h,cpp}`（手势判定、滚轮累计、百分比/VCP 换算、暖色 LUT、显示器列表归一化、`bright` 查询解析），不引 OS 头文件，可单测。
- 偏差：第 6 节写的「会话切换（WTS 事件）时恢复」未实现——未注册 WTS 会话通知。进程退出恢复已覆盖主要场景；锁屏/切换期间 ramp 的实际行为列入 `docs/tests.md` 第 3 节 E2E 待验，若实测发现解锁后滤镜丢失，再补 WTS 监听。

### 实测

- 单元测试：`brb_tests` 52 条全绿（v0.1.0 时；v0.2.0 已扩到 68 条）。
- 协议往返：`initialize → query(留空) → query(70) → query(abc) → query(105) → action(up) → shutdown` 在本机实跑，返回结构与文案符合规格；不可调显示器返回 `外屏 · 不支持`，`bright 70` / `105` 返回 `no-display` 提示行，动作返回 `notify`。
- 状态持久化：`state.json` 读写路径已接通（`WS_DATA_DIR`），未在真机上验证重启恢复。
- 真实宿主（2026-11-08，本机单屏不可调）：安装到 `%LOCALAPPDATA%\WinShun\plugins\ra9fael.bright\` 并在 `WinShun.ini` 写入 `[Plugin.ra9fael.bright]` 启用与权限确认后，宿主热加载成功（plugins.log 记录启动，无超时/错误/退出行）。`WinShun.exe --query "bright"` 打开输入框回车执行：结果列出「Generic PnP Monitor / 外屏 · 不支持 · \\.\DISPLAY65」（屏幕亮度标签），与规格第 10 节失败矩阵一致。常驻进程 12.5 MB（限额 256 MB），全程无重启。

### 未验证（发布阻塞项）

- **测试环境是 mstsc 远程会话**（2026-11-10 确认）：会话里 `EnumDisplayMonitors` 只有一块 `\\.\DISPLAY97` 虚拟屏，物理屏（`WmiMonitorID` 能看到 BELLATOR N27Q / DELL U2722DX）不在会话的显示子系统里，`WmiMonitorBrightness` 为空、DDC VCP 0x10 全部不应答。这不是驱动或硬件故障，而是 RDP 的固有限制（Twinkle Tray 等工具同样对 RDP 禁用亮度）。v0.2.0 起插件用 `isRemoteSession()`（`SM_REMOTESESSION` + 窗口站名）主动识别并在列表/浮层说明。
- 因此以下只在代码层面成立，**必须在控制台会话复测**：
  - 内屏 WMI 读写、外屏 DDC 读写与 OSD 出现；
  - VCP max≠100 的换算与 800ms 回读校准（±2%）；
  - 暖色滤镜的视觉效果与锁屏/解锁恢复；
  - 手势与新快捷键的真实按键路径（本机只验证到 JSON-RPC 层）；
  - 鼠标所在屏选屏的正确性（本机只有一块虚拟屏，选屏由单测覆盖）；
  - 双屏拔插后的列表重建。
- 这些需要在 Windows 11 双屏实机、按 `docs/tests.md` 第 3 节逐条勾选后才能宣布可发布。

## 13 实现记录（v0.2.0 / v0.3.0）

针对"不是常驻 / 只能在任务栏 / 只能调一块屏 / 快捷键不可配"四个反馈的改动；v0.3.0 追加软件亮度模拟与隐藏效果。

### 根因

| 现象 | 根因 | 证据 |
|---|---|---|
| 10 分钟后手势失效 | `plugin.json` 未写 `idleExitSec`，宿主按默认 600s 回收 | `plugins.log` 每段「启动 → +600s 空闲，退出」 |
| 只在任务栏有效 | 钩子本身全局；进程被回收后完全无响应 | `fastGate()` 只看修饰键，不看坐标 |
| 只有一块屏能调 | DDC 首次读失败即 `adjustable=false`，直到 `WM_DISPLAYCHANGE` 才重试；远程会话下必然如此 | `bindDdc()` 只探测一次 |
| 快捷键不可配 | 未实现；修饰键硬编码 | `decideGesture()` 旧签名无 chord 参数 |

### 改动

- `plugin.json`：`idleExitSec: 0`；新增 `gestureModifiers`、`gestureTarget`、`hotkeyUp/Down/Max`、`gestureDebug`、`overlayAnchor: target-monitor`。
- `bright_logic`：`Rect`、`MonitorInfo.bounds`、`monitorIndexAtPoint()`（含最近屏回退）、`GestureModifiers`（parse/text/matches）、`decideGesture(e, chord)`、`GestureDecision.x/y`。空 chord 永远匹配不到，避免吞掉全部滚轮。
- `mouse_hook`：chord 用原子掩码热更新；坐标随事件传出；每 10s 卸载重装 `WH_MOUSE_LL` 做健康检查。
- `display_controller`：`rcMonitor` 填 `bounds`；DDC 探测 3 次重试；失败保留句柄为 `bound=false`，首次使用时惰性重绑；`lastError()` 给出稳定错误键；`isRemoteSession()`。
- `bright_worker`：`BrightOp` 带 `target/x/y`；按屏或全部；浮层跟随被调屏；回读校准只比对本次写入的屏；远程会话专用文案。
- `hotkey_thread`：宿主无 `hostHotkeys` 时的回退实现（自建消息队列 + `RegisterHotKey`）。
- `winshun.h`：保存 `initialize.capabilities`、`capabilities()/hostHotkeys()`，新增 `hotkey` 通知分发与 `virtual void hotkey(...)`。
- 线程安全：`dispatchGesture` 改为加锁快照设置（原实现在钩子线程无锁读 `m_settings`）。
- v0.3.0：新增 `dim_layer.{h,cpp}`（每屏一个 click-through 遮罩窗口）、`isNight`/`EggTracker`/`eggFrame` 三个纯函数、`overlay.showGlyphCount()`、`settings.h` 的 `SoftwareDim` 三态与 `softwareMaxDark`/`mushroomEgg`、`bright_worker` 的 `dimState`/`refreshDim`/`eggTick`。

### 实测（v0.3.0）

- 单测 84 条全绿（新增 16 条：`dimAlpha` 端点/单调/夹取、`softwareBrightness` 三态、`softwareMaxDark` 0–90、`mushroomEgg`、`isNight` 全部边界与回绕、`EggTracker` 触发/断档/下滚/换屏/非 100%/白天/开关、`eggFrame` 起止/渐变/色域/倒计时/淡出）。
- 软件遮罩真机验证：`FindWindow("WinShunBrightDim")` 命中，矩形 `0,0 1920x1080`，扩展样式 `0x80800A8`（LAYERED|TRANSPARENT|NOACTIVATE|TOOLWINDOW|TOPMOST），`GetLayeredWindowAttributes` alpha=46 与 `dimAlpha(70,60)` 一致；100% 时不建窗口。
- 隐藏效果端到端：临时把夜间判定固定到 23:00（验证后已移除该钩子），持续上滚 10 秒后 alpha 采样 15→22→67→103→125→160→202，倒计时结束回落至 0、日志 `egg over, restored 100%`；白天与 `mushroomEgg:false` 情形 alpha 恒为 15。
- 实测中修掉三个缺陷：彩蛋期间手势会把动画 alpha 打回静态值；浮层每 100ms 被重推一次导致不消失；回到 100% 时只隐藏窗口残留旧 alpha，下次显示会带着旧暗度回来。
- 协议往返（Node 驱动真实进程）：`initialize → query → settingsChanged → hotkey → shutdown`；`register_hotkeys` 依次收到三键、改键后重新注册、退出 `[]`；无 `hostHotkeys` 时走自注册且 `hotkey` 通知能驱动 worker（stderr 显示远程会话文案）。
- 远程会话识别生效：列表追加 `remote-session` 行，`bright 70` 提示行改为远程文案。
- 未做：控制台会话下的真实亮度验证（见上节）。
