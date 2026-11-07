# ra9fael.bright 功能规格（v0.1.0）

状态：设计完成，代码未实现。协议与清单字段以 WinShun 0.22.0 插件文档为准（resident JSON-RPC over stdio）。

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

- **Ctrl+Alt+滚轮**（任意位置）：同时调所有可调显示器 ±N%，浮层显示平均值
  - 修饰键必须是且仅是 Ctrl+Alt；只 Ctrl（volume 手势）、只 Alt、含 Shift/Win、组合不全 → 一律透传
  - SendInput 注入的滚轮 → 透传，防自触发循环
  - 无可调显示器 → 吞掉事件并浮层说明
- **Ctrl+Alt+左键**：暖色滤镜开/关（`warmStrength` > 0 时生效），浮层显示当前状态
- 事件被识别但无目标时：吞掉 + 浮层解释（同 volume 的 "Recognized, no target" 行）

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

## 7 浮层

复用 volume 浮层实现：九宫格锚点 + 偏移、非激活、自动消失、DPI 感知。显示 `<显示器名或"全部"> NN%` + 进度条；暖色切换时显示开/关一行文案。

## 8 权限

- `ui`：低级鼠标钩子 + 浮层窗口
- `system:settings`：亮度属系统设置面（WMI/DDC/gamma 本身不需要提权）
- 不做 `elevated`、`network`、`clipboard`

## 9 数据位置

默认不持久化；`warmRemember` 时仅 `WS_DATA_DIR\state.json`（int 强度 + bool 开关）。

## 10 失败处理

- 无 WMI 亮度类 → 该行走 DDC 尝试；仍不支持则标「不支持」
- DDC 不响应（线材/坞站不支持）→ 标「不支持」，动作给出说明
- 全部不支持 → 留空查询返回提示行，手势吞掉并解释
- `SetVCPFeature` 失败 → 浮层/通知报错，不假装成功
- gamma ramp 被其他程序接管（f.lux 类）→ 后写者生效，不轮询抢夺
- 低级钩子被系统移除（超时）→ 参照 volume：下次手势时重建一次，连续失败禁用手势并提示

## 11 里程碑

- v0.1：上述全部（DDC + WMI + 滤镜 + 手势 + 浮层）
- v0.2 候选：HDR 开关、按鼠标所在屏幕独立调节、系统夜灯状态只读检测
