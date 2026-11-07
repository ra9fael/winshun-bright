# ra9fael.bright 发布与安装流程

## 构建

参照 bosskey/volume 架构：`bright.exe` 薄引导器 + `BrightApp.dll`（QML 浮层，链接宿主共享 Qt 运行时，`WS_QT_DIR` 指向 D:\tools\Qt\6.12.0\msvc2022_64），Ninja Release。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物落在仓库根（与 plugin.json 同目录）。内存上限 `memoryMB: 256`——浮层走宿主渲染路径，双屏 D3D11 需约 210MB，勿再下调。

## 打包（.wsplugin）

1. 确认 `plugin.json` 的 `version` 与发布 tag 一致
2. `dist/ra9fael.bright-<ver>.wsplugin` = zip，包含且仅包含：
   - `plugin.json`
   - `bright.exe`
   - `BrightApp.dll`
   - `README.md`
   - `LICENSE`
3. 计算 SHA-256：`Get-FileHash dist\ra9fael.bright-<ver>.wsplugin`

## 发布

1. `git tag v<ver> && git push origin main --tags`
2. GitHub Release `v<ver>`：上传 `.wsplugin`；release notes 写变更点、SHA-256、构建命令与硬件要求（外屏需支持 DDC/CI，内屏需驱动提供 WMI 亮度类）
3. 声明为社区原生包（`unreviewed-native`）

## 安装（用户）

- 方式 A（宿主界面）：Win顺 → 插件管理 → 本地安装 → 选择 `.wsplugin` → 确认（unreviewed-native 提示）→ 停用再启用插件（或重启宿主）
- 方式 B（手动）：解包到 `%LOCALAPPDATA%\WinShun\plugins\ra9fael.bright\`（`plugin.json` 必须在目录根部）→ 重启 Win顺
- 首次启用弹权限确认（窗口/钩子、系统设置）；eager 启动，宿主启动即常驻
- 注意：若插件目录是 Junction 指向 worktree（开发机场景），切换主目录分支会影响已装插件——发布机不要用 Junction

## 升级

- 覆盖安装新版 `.wsplugin`，或替换目录内文件后停用/启用插件
- `state.json`（warmRemember）跨版本保留，无需迁移

## 卸载

- 宿主中停用 + 删除，或直接删除插件目录后重启宿主
- 残留：暖色滤镜若开着，卸载前请先关闭滤镜（进程退出时也会恢复原始 ramp 兜底）；`state.json` 随目录删除

## 回滚

- GitHub Release 保留历史版本；下载旧 `.wsplugin` 覆盖安装即可
