# WINOME

<img width="1920" height="1080" alt="图片" src="https://github.com/user-attachments/assets/be68a00c-bf82-4718-9add-70cc17413c2e" />

在 Microsoft Windows 上用 Windows 原生功能支撑的 GNOME Shell UI 复刻。

## 仓库结构

```
.
├── WINOME/          # 主工程（meson 构建根，C++ 宿主 + GTK4 UI + St 引擎）
├── upstream/        # 上游完整仓库（独立 git，仅供参考/更新提取）
│   ├── gnome-shell/   # GNOME Shell（GPL-2.0-or-later）
│   ├── Seelen-UI/     # 启发实现（AGPL-3.0-or-later，仅致谢，不派生）
│   └── WinOverview/   # GNOME 概览复刻（GPL-2.0）
├── extracted/       # 从上游提取、经 WINOME 修改的模块（去掉了上游 git）
│   ├── WinOverview/   # 概览（DLL 注入 + DWM 缩略图 + workspace.js 布局）
│   └── GMONE-Shell/   # Shell 外观（Sass 主题 + SVG）+ 布局/日期/动画算法
├── ENV.md           # 开发环境说明（MSYS2 UCRT64 + GTK4）
└── LICENSE / README.md
```

- **宿主进程**：`WINOME/src/`（C++ + GTK4）
- **St CSS 引擎复刻**：`WINOME/st-compat/`（libcroco + st-theme-node，去 Clutter/Cogl）
- **隐藏/替换 Windows 原生界面**：`WINOME/src/`（`native-taskbar.cpp` + `win-event-hook.cpp`，独立实现，思路受 Seelen-UI 启发）
- **概览（Activities）**：`extracted/WinOverview/`
- **外观与布局算法参考**：`extracted/GMONE-Shell/`

## 构建（MSYS2 UCRT64）

```bash
cd WINOME
meson setup build
ninja -C build
./build/src/winome.exe
```

## 当前状态

- [x] 宿主进程骨架 + 置顶全宽顶栏（Activities 圆点 + 时钟 + 状态区图标 + 悬停/点击动画）
- [x] 隐藏原生任务栏、桌面层嵌入 WorkerW、事件钩子防重现（独立实现）
- [x] 接入 WinOverview 概览（Win+W 触发）
- [x] 快速设置面板：GNOME 完整布局（电量/音量/亮度 + 8 个开关）
- [x] 真实数据源：电量、音量、网络、锁屏
- [x] **St CSS 引擎完整复刻**（`st-compat/`）：运行时精确查询任意 GNOME 样式属性
- [ ] 日历面板完整实现、关机菜单、网络/蓝牙/亮度真实数据

## 架构说明

详见 [WINOME/README.md](WINOME/README.md)。

## 权限要求

- 隐藏任务栏、操作 Explorer 窗口需要**管理员权限**运行 `winome.exe`

## 许可

GPL-2.0，详见 `LICENSE` 与 `WINOME/THIRD-PARTY-NOTICES`。
