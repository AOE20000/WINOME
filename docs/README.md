# WINOME 开发文档中心

本目录统一收口 WINOME 的项目文档、架构设计、构建说明、踩坑记录与阶段开发日志。

## 文档导航

| 文档 | 内容说明 |
|------|----------|
| [build-and-env.md](build-and-env.md) | MSYS2 UCRT64 开发环境、`build.sh` 三方案构建与已知构建问题 |
| [architecture.md](architecture.md) | 宿主启动流程、任务栏隐藏、顶栏/弹出窗、Activities 概览、虚拟桌面 COM、数据源与性能基线 |
| [theme-and-ui.md](theme-and-ui.md) | 双 CSS 管线（`st-compat` + `convert-st-css.py`）、构建期自检、Clutter 动效层与快速设置菜单规范 |
| [pitfalls.md](pitfalls.md) | 工程硬约束、Win32/GTK4/GCC LTO 踩坑汇总、样式排查方法论与已评估暂缓项 |
| [roadmap.md](roadmap.md) | 生产级完善开发计划（P0–P5 六阶段路线图、验收标准与已交付汇总） |
| [long-term-plan.md](long-term-plan.md) | 长期演进蓝图（Shell 闭环、性能事件化、窗口管理与文件/媒体探索参考） |
| [file-manager-research.md](file-manager-research.md) | Nautilus / Windows 原生文件管理方案调研参考 |
| `dev-logs/` | 阶段调研与排障日志归档：[2026-10-11](dev-logs/2026-10-11.md)、[2026-10-13](dev-logs/2026-10-13.md)、[2026-11-16](dev-logs/2026-11-16.md) |

## 项目开发约定

1. **技术路线**：在 Windows 上用原生 Win32/COM 能力支撑的 GNOME Shell UI 复刻。宿主单进程 `winome.exe`（C++17 + GTK4），无注入、无核心 UI DLL；核心模块按 `upstream/gnome-shell` 逐组件移植，`upstream/` 与 `extracted/` 用作只读/派生参考（其中 `upstream/Seelen-UI` 受 AGPL 约束仅作功能思路致谢，不派生其代码）。
2. **注释与文档语言**：源码注释使用英文；技术散文与开发文档使用中文。
3. **模块职责边界**：
   - 概览（含多屏同时概览）、动态壁纸同步、应用索引与分页网格 → `WINOME/src/overview*.cpp`、`overview-secondary.cpp`、`live-wallpaper.cpp`、`app-index.cpp`、`app-grid.cpp`
   - 顶栏、工作区保留与弹出窗窗口壳 → `WINOME/src/shell-panel.cpp`
   - 多显示器拓扑、全屏与锁屏监测 → `WINOME/src/monitor-manager.cpp`、`fullscreen-watcher.cpp`
   - 快速设置面板与系统托盘 → `WINOME/src/quick-settings.cpp`、`systray.cpp`
   - 日历、通知中心与媒体卡片 → `WINOME/src/calendar.cpp`、`notifications.cpp`、`winrt-bridge.cpp`
   - 多屏独立工作区引擎与虚拟桌面 COM → `WINOME/src/workspace-manager.cpp`、`virtual-desktop.cpp`
   - 系统数据源 → `WINOME/src/system-status.cpp`、`wifi-networks.cpp`、`power-modes.cpp`
   - 配置、国际化与设置窗口 → `WINOME/src/config.cpp`、`i18n.cpp`、`settings-window.cpp`
   - 宿主通信、遥测/安全、崩溃弹窗、一键排障压缩包导出、调试台、运行弹窗与插件宿主机 → `WINOME/src/shell-ipc.cpp`、`perf-metrics.cpp`、`crash-dialog.cpp`、`diagnostics-export.cpp`、`debug-runtime.cpp`、`looking-glass.cpp`、`run-dialog.cpp`、`plugin-host.cpp` 及非提权控制端 `WINOME/ctl/winome-ctl.cpp`
   - 会话确认弹窗与 DWM 窗口统一样式 → `WINOME/src/end-session-dialog.cpp`、`window-theming.cpp`
4. **构建清单同步**：新增源码文件必须同步加入 [WINOME/src/meson.build](../WINOME/src/meson.build) 的 `winome_sources`；引入新的 Win32 系统库须同步添加 `cc.find_library(...)`。
5. **状态清单单一事实源**：功能完成状态统一维护在根目录 [README.md](../README.md)，避免多处 README 各自维护导致漂移。
6. **数据刷新模型**：WASAPI 音量、`WlanRegisterNotification` Wi-Fi、显示器/DPI/工作区（`SPI_SETWORKAREA`）与最小化结束（`EVENT_SYSTEM_MINIMIZEEND`）、以及热角（`WH_MOUSE_LL`，全屏隐藏时自动卸钩）均采用事件驱动回调；虚拟桌面通过单一共享 WinEvent + 快捷键提示 + `1.5s` 安全校对器广播同步；通知中心后台可 join 工作线程按活动/闲置自适应间隔（`2s`/`6s`）同步；全屏巡检与 z-order 兜底周期放宽至 `10s`。
