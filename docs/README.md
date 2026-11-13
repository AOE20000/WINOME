# WINOME 开发文档中心

本目录统一收口 WINOME 的项目文档、架构设计、构建说明、踩坑记录与阶段开发日志（替代原 `.workbuddy/` 记忆文件、根目录散落的 `ENV.md` 以及原 `WINOME/README.md` 中的长篇技术笔记）。

## 文档导航

| 文档 | 内容说明 |
|------|----------|
| [build-and-env.md](build-and-env.md) | MSYS2 UCRT64 开发环境、`build.sh` 三方案构建与已知构建问题 |
| [architecture.md](architecture.md) | 宿主启动流程、任务栏隐藏、顶栏/弹出窗、Activities 概览、虚拟桌面 COM、数据源与性能基线 |
| [theme-and-ui.md](theme-and-ui.md) | 双 CSS 管线（`st-compat` + `convert-st-css.py`）、构建期自检、Clutter 动效层与快速设置菜单规范 |
| [pitfalls.md](pitfalls.md) | 工程硬约束、Win32/GTK4/GCC LTO 踩坑汇总、样式排查方法论与已评估暂缓项 |
| `dev-logs/` | 阶段调研与排障日志归档：[2026-10-11](dev-logs/2026-10-11.md)、[2026-10-13](dev-logs/2026-10-13.md) |

## 项目开发约定

1. **技术路线**：在 Windows 上用原生 Win32/COM 能力支撑的 GNOME Shell UI 复刻。宿主单进程 `winome.exe`（C++17 + GTK4），无注入、无 DLL；源码以逐组件移植 `upstream/gnome-shell` 的 JS/CSS 为准（`upstream/` 仅作只读参考，不派生发布）。
2. **注释与文档语言**：源码注释使用英文；技术散文与开发文档使用中文。
3. **模块职责边界**：
   - 概览相关 → `WINOME/src/overview*.cpp`
   - 顶栏、工作区保留与弹出窗窗口壳 → `WINOME/src/shell-panel.cpp`
   - 快速设置面板与二级菜单内容 → `WINOME/src/quick-settings.cpp`
   - 日历控件 → `WINOME/src/calendar.cpp`
   - 虚拟桌面 COM 与窗口固定 → `WINOME/src/virtual-desktop.cpp`
   - 系统数据源 → `WINOME/src/system-status.cpp`、`wifi-networks.cpp`、`power-modes.cpp`
4. **构建清单同步**：新增源码文件必须同步加入 [WINOME/src/meson.build](../WINOME/src/meson.build) 的 `winome_sources`；引入新的 Win32 系统库须同步添加 `cc.find_library(...)`。
5. **状态清单单一事实源**：功能完成状态与待办清单统一维护在根目录 [README.md](../README.md)，避免多处 README 各自维护导致漂移。
6. **常用轮询节拍**：状态区图标 `2s`、`fullscreen-watcher` `400ms`、OSD / 虚拟桌面切换检测 `400ms`（桌面计数每约 `5s` 刷新一次）、左上角热角 `100ms`。
