# ra9fael.bright 测试规格

## 1 单元测试（纯逻辑，ctest 目标 `brb_tests`）——已实现，84 条全绿

- 滚轮累计：沿用 volume 的 wheel accumulation 用例结构（跨多格累计、反向、单格）
- 百分比数学：clamp 0–100、步进溢出（98 +10 → 100）、VCP max≠100 换算与回读反算
- 修饰键判定表：配置的组合通过；仅 Ctrl（volume 场景）/ 仅 Alt / 含多余 Shift 或 Win / 组合不全 → 透传；SendInput 注入 → 透传
- 修饰键解析：`ctrl+alt`↔`alt+ctrl` 归一、`win+alt`、`ctrl+shift+alt`、`meta`→`win`；未知名与空组合回默认（空组合必须匹配不到任何东西，否则会吞掉所有滚轮）
- 选屏：点在左/右屏、两屏缝隙取最近、屏幕外仍落到最近一块、不可调屏跳过、全不可调返回 -1；`Rect::contains` 半开区间与 `distanceSquared`
- 手势坐标随判定传出
- 设置解析：`percentPerStep` 1–10、`overlayMs` 300–3000、`warmStrength` 0–100 越界回默认；`overlayAnchor` 非法值回默认（含新 `target-monitor`）；`gestureTarget` 默认与非法值；三个 hotkey 读入
- 暖色 LUT 生成：强度 0 恒等曲线、100 最深、值域 clamp、单调性（R 不减、G/B 不增）
- 显示器列表去重与排序稳定；热插拔重建逻辑（快照注入模拟）
- `bright` 查询解析：留空列表 / 数字设置 / 非法输入提示
- 软件遮罩：`dimAlpha` 的端点与单调性、`softwareMaxDark` 0–90 clamp、`softwareBrightness` 三态解析
- 蘑菇彩蛋：`isNight` 边界（21:59/22:00/05:59/06:00/跨零点/越界回绕）、`EggTracker`（连续 4 秒触发、断档 >1.5 秒重来、下滚清零、非 100%、白天、开关关闭、换屏）

目标 ≥40 用例，实际 84 条（`tests/test_bright_logic.cpp`，极简 TEST/CHECK 框架见 `tests/test_harness.h`）。

## 2 协议往返（真实进程）——本机已实跑

- `initialize → query → settingsChanged → hotkey → shutdown` 全链路 JSON-RPC
- 留空查询返回全部显示器行（`data` 带 `bounds` / `percent` / `adjustable`）；`bright 70` 返回每台结果行
- 热键两条路径：`capabilities.hostHotkeys: true` → `register_hotkeys` 收到三个键、改设置后重新注册、退出注册 `[]`；无该能力 → 自建 `RegisterHotKey` 线程，`hotkey` 通知能驱动 worker
- 远程会话：列表追加 `remote-session` 说明行，`bright 70` 的提示行改为远程文案

2026-11-10 实跑（mstsc 远程会话，headless）：`initialize → query(留空) → query(70) → settingsChanged → hotkey → shutdown` 全部通过；`register_hotkeys` 与回退路径均验证。软件遮罩在真机上确认：`FindWindow("WinShunBrightDim")` 命中窗口、矩形 `0,0 1920x1080`、扩展样式 `0x80800A8`（LAYERED|TRANSPARENT|NOACTIVATE|TOOLWINDOW|TOPMOST）、`GetLayeredWindowAttributes` 实测 alpha 46 与 `dimAlpha(70,60)` 一致；100% 时不建窗口。浮层呈现与 `timeoutMs` 边界由宿主侧计时，归入第 3 节 E2E。

## 3 手动 E2E（**必须在本机会话**，Windows 11，宿主 0.22.0+，双屏：内屏 + 支持 DDC 的外屏）

远程桌面（mstsc）下硬件通道相关的项无法验证：会话只有一块虚拟屏，WMI/DDC/gamma 都到不了物理显示器（插件会主动提示）。软件遮罩与蘑菇浮层动画不依赖物理屏，远程会话内也可验证（标 ★ 的项）。

- [ ] 进程常驻：启动后放置 30 分钟不做任何操作，`plugins.log` 不出现「空闲，退出」
- [ ] `bright` 留空：两行正确标注内屏/外屏与通道、当前亮度与系统滑杆一致
- [ ] 单行 Enter / Ctrl+Enter / Shift+Enter：仅该台变化，外屏出现 OSD
- [ ] `bright 70`：两台同时到 70（回读校准 ≤2% 误差）
- [ ] 鼠标在左屏 Ctrl+Alt+滚轮 → 只左屏变化、浮层在左屏；移到右屏同理
- [ ] 改成 `gestureTarget: all` → 两台同时变化，浮层显示「全部显示器」
- [ ] 改成 `gestureModifiers: alt+shift` → 新组合生效，旧 Ctrl+Alt 变成透传
- [ ] 设置三个 hotkey → 按一次亮度变化，改组合后立即生效，清空后不再响应
- [ ] 任务栏上只按 Ctrl+滚轮 → volume 生效，bright 不动（互不抢事件）
- [ ] Ctrl+Alt+点击：暖色开/关与浮层提示；关闭后截图对比颜色还原
- [ ] 暖色开启时锁屏再解锁 → ramp 恢复正常或滤镜恢复
- [ ] 拔掉外屏 → 列表只剩内屏，手势仍工作；插回恢复（首次 DDC 读失败的那块会在首次手势时重新绑定）
- [ ] 与 f.lux 同开 → 无崩溃，后写者生效
- [ ] `bright 105` → clamp 到 100 并注明；`bright abc` → 提示行
- [ ] `gestureDebug: true` → plugins.log 出现钩子安装/重装、判定坐标、`soft dim ... alpha=`
- [ ] 中英文 locale

### 软件亮度模拟（★ 远程会话可验）

- [ ] ★ `softwareBrightness: unsupported-only` → `bright` 里不可调的那行变「软件模拟」，不标无效
- [ ] ★ 在该屏滚轮 → 屏幕可见变暗，浮层显示百分比；再往回滚恢复透亮
- [ ] ★ `softwareMaxDark: 90` → 0% 时几乎全黑但仍能看见周围；`0` → 完全不变暗
- [ ] ★ `softwareBrightness: always` → 硬件可调的屏也叠加变暗（双重变暗）
- [ ] ★ 遮罩不挡点击：在变暗区域点桌面图标、拖窗口、右键菜单均正常
- [ ] ★ 遮罩不出现在任务栏与 Alt+Tab
- [ ] ★ 改分辨率/拔插屏 → 遮罩跟随新位置重建（不变形、不残留）
- [ ] ★ 关闭插件或禁用 → 遮罩立即消失，屏上无残留
- [ ] 硬件可调的屏 + `unsupported-only` → 仍走 WMI/DDC，不被遮罩覆盖

### 蘑菇彩蛋（★ 远程会话可验）

- [ ] ★ `mushroomEgg: true` 下，在同一屏持续向上滚动约 4 秒（不受时间和亮度限制，“全部显示器”目标也可触发）→ 全屏特效开始随机循环
- [ ] ★ 彩蛋窗口覆盖且仅覆盖鼠标所在显示器，emoji/粒子分布到屏幕范围，未绘制区域透明；亮度数值与遮罩状态不变
- [ ] ★ 未修饰的鼠标左键点击关闭彩蛋且点击穿透；Ctrl+Alt+左键同时关闭彩蛋并切换暖色滤镜
- [ ] ★ 动画期间关闭 `mushroomEgg`、拔插显示器或切换会话 → 全屏特效立即隐藏
- [ ] ★ 持续滚动及 800ms 回读校准同时进行 → 亮度提示与全屏特效各自正常，不互相替换
- [ ] ★ 中途断档 >1.5 秒 / 下滚 / 换屏 → 不会触发
- [ ] ★ 白天触发同样操作 → 不会触发
- [ ] ★ `mushroomEgg: false` → 做同样操作不触发

## 4 验收标准

- 单测全绿；协议往返通过；第 3 节（本机会话）E2E 全勾
- 连续 30 分钟手势与浮层操作，任务管理器观察无内存/句柄增长
