# ra9fael.bright 测试规格

## 1 单元测试（纯逻辑，ctest 目标 `brb_tests`）——已实现，52 条全绿

- 滚轮累计：沿用 volume 的 wheel accumulation 用例结构（跨多格累计、反向、单格）
- 百分比数学：clamp 0–100、步进溢出（98 +10 → 100）、VCP max≠100 换算与回读反算
- 修饰键判定表：仅 Ctrl+Alt 通过；仅 Ctrl / 仅 Alt / 含 Shift / 含 Win / 组合不全 → 透传；SendInput 注入 → 透传；任务栏上只 Ctrl（volume 场景）→ 透传给 volume
- 设置解析：`percentPerStep` 1–10、`overlayMs` 300–3000、`warmStrength` 0–100 越界回默认；`overlayAnchor` 非法值回默认
- 暖色 LUT 生成：强度 0 恒等曲线、100 最深、值域 clamp、单调性（R 不减、G/B 不增）
- 显示器列表去重与排序稳定；热插拔重建逻辑（快照注入模拟）
- `bright` 查询解析：留空列表 / 数字设置 / 非法输入提示（实现时新增）

目标 ≥40 用例，实际 52 条（`tests/test_bright_logic.cpp`，极简 TEST/CHECK 框架见 `tests/test_harness.h`）。

## 2 协议往返（真实进程）——本机已实跑

- `initialize → query → action（up/down/max）→ shutdown` 全链路 JSON-RPC
- 留空查询返回全部显示器行；`bright 70` 返回每台结果行
- `settingsChanged` 后手势步进与浮层行为立即生效
- cancel / `timeoutMs` 1500 边界

2026-11-08 实跑（本机单屏不可调，headless）：`initialize → query(留空/70/abc/105) → action(up) → settingsChanged → appearance → shutdown` 全部通过，失败矩阵文案符合 spec 第 10 节。浮层呈现与 `timeoutMs` 边界由宿主侧计时，归入第 3 节 E2E。

## 3 手动 E2E（Windows 11，宿主 0.22.0+，双屏：内屏 + 支持 DDC 的外屏）

- [ ] `bright` 留空：两行正确标注内屏/外屏与通道、当前亮度与系统滑杆一致
- [ ] 单行 Enter / Ctrl+Enter / Shift+Enter：仅该台变化，外屏出现 OSD
- [ ] `bright 70`：两台同时到 70（回读校准 ≤2% 误差）
- [ ] Ctrl+Alt+滚轮：浮层出现、百分比正确、松手 800ms 回读无跳变
- [ ] 任务栏上只按 Ctrl+滚轮 → volume 生效，bright 不动（互不抢事件）
- [ ] Ctrl+Alt+点击：暖色开/关与浮层提示；关闭后截图对比颜色还原
- [ ] 暖色开启时锁屏再解锁 → ramp 恢复正常或滤镜恢复（行为与 spec 一致）
- [ ] 拔掉外屏 → 列表只剩内屏，手势仍工作；插回恢复
- [ ] 与 f.lux 同开 → 无崩溃，后写者生效
- [ ] `bright 105` → clamp 到 100 并注明；`bright abc` → 提示行
- [ ] 中英文 locale

## 4 验收标准

- 单测全绿；协议往返通过；E2E 全勾
- 连续 30 分钟手势与浮层操作，任务管理器观察无内存/句柄增长
