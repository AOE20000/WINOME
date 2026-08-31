# WINOME

<img width="1920" height="1080" alt="图片" src="https://github.com/user-attachments/assets/a38220d2-7f62-4b80-9da8-c73457b3b63c" />


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
│   ├── WinOverview/   # 旧概览方案（DLL 注入，已退役，仅参考）
│   └── GMONE-Shell/   # Shell 外观（Sass 主题 + SVG）+ 布局/日期/动画算法
├── build.sh         # 交互式构建程序（生产/测试/调试方案）
├── ENV.md           # 开发环境说明（MSYS2 UCRT64 + GTK4）
└── LICENSE / README.md
```

- **宿主进程**：`WINOME/src/`（C++ + GTK4）
- **概览（Activities）**：`WINOME/src/overview.cpp` 宿主进程内 GTK4 渲染 + DWM 缩略图（旧 DLL 注入方案已退役）
- **St CSS 引擎复刻**：`WINOME/st-compat/`（libcroco + st-theme-node，去 Clutter/Cogl）
- **隐藏/替换 Windows 原生界面**：`WINOME/src/`（`native-taskbar.cpp` + `win-event-hook.cpp`，独立实现，思路受 Seelen-UI 启发）
- **虚拟桌面**：`WINOME/src/virtual-desktop.cpp`（移植 VirtualDesktopAccessor-rust 的 COM 接口）
- **外观与布局算法参考**：`extracted/GMONE-Shell/`

## 构建（MSYS2 UCRT64）

推荐使用根目录的交互式构建程序：

```bash
./build.sh          # 交互式菜单
./build.sh release  # 或直接指定方案
```

| 方案 | 配置 | 构建目录 | 说明 |
|------|------|----------|------|
| `release` 生产 | release + LTO + NDEBUG | `build-release/` | 最小体积、最快指令（约 1.3MB） |
| `test` 测试 | debugoptimized | `build/` | 项目默认，保留 `g_print` 诊断日志 |
| `debug` 调试 | debug（-O0 -g） | `build-debug/` | GDB 友好 |

其他子命令：`./build.sh run`（运行最近构建）、`./build.sh clean`（清理全部构建目录）。三方案独立目录，切换互不触发重编。

也可手动构建（等价于测试方案）：

```bash
cd WINOME
meson setup build
ninja -C build
./build/src/winome.exe
```

## 当前状态

- [x] 宿主进程骨架 + 置顶全宽顶栏（Activities 圆点 + 时钟 + 状态区图标 + 悬停/点击动画）
- [x] 隐藏原生任务栏、桌面层嵌入 WorkerW、事件钩子防重现（独立实现）
- [x] 接入 WinOverview 概览（**Win 键**、**左上角热角**触发，`Win+Tab` 打开开始菜单，其余 Win 组合键透传）
- [x] 概览显示**最小化窗口**；面板在概览上方保持可见（概览让出顶栏条带）
- [x] 顶栏保留工作区：最大化/全屏应用停在面板下方（像任务栏保留底部）
- [x] 面板全屏自动隐藏（真全屏/锁屏），桌面/最大化/Alt-Tab 切换器下保持显示
- [x] 面板隐藏于 Alt-Tab 与任务栏（`WS_EX_TOOLWINDOW`）
- [x] 宿主为 GUI 程序（无终端窗口），stdout/stderr 写入 `%TEMP%\winome.log`
- [x] 快速设置/日历弹出窗重新接入：点击状态区打开 GNOME 快速设置（电量+操作按钮、音量/亮度滑块、8 个开关胶囊），点击时钟打开日历；弹出窗为自绘无边框顶层窗口，面板下方 + 屏幕侧边留有稳定间隙，概览打开时自动关闭、在概览上打开时置顶
- [x] 概览层级集成：顶栏与弹出窗浮于概览之上（概览让出顶栏条带），概览打开时顶栏底色变为透明（GNOME 风格），面板/弹出窗上的点击不会让概览误判为空白而关闭
- [x] 真实数据源：电量（`GetSystemPowerStatus`）、音量（WASAPI，滑块实时调节 + 图标动态）、网络（`InternetGetConnectedState`）、锁屏（`LockWorkStation`）
- [x] **Windows 虚拟桌面兼容**：Win11 22621/24H2 COM 接口枚举/切换桌面、窗口归属查询；概览按桌面过滤窗口、工作区缩略图行点击切换桌面
- [x] **原生概览重写**：overviewControls.js WINDOW_PICKER 布局逐组件移植（搜索栏/工作区背景/窗口网格/dash/悬停 chrome），DWM 缩略图飞入动画
- [x] 性能优化：动画走帧时钟（`gtk_widget_add_tick_callback`，vblank 对齐）、z-order 重排漂移门控（空闲零 `SetWindowPos` churn）、状态图标跳过无谓重绘、发布构建 LTO + NDEBUG（详见 [WINOME/README.md](WINOME/README.md) 性能章节）
- [ ] 二级菜单（WLAN/蓝牙/电源模式/关机菜单）、日历面板重构、网络/蓝牙真实数据
- [ ] 概览搜索（应用/窗口检索）

## 快捷键

| 按键 | 行为 |
|------|------|
| **Win**（单独按下松开） | 切换概览（Activities） |
| **鼠标移到左上角热角** | 打开概览 |
| **Win+Tab** | 打开开始菜单 |
| **Win+E / Win+R / Win+D …** | 透传给 Windows（组合键保留） |
| **Win+Alt+Tab** | 任务视图（透传） |
| **Win+Ctrl+←/→** | 切换虚拟桌面（透传；概览打开时自动跟随重排） |
| **Alt+Tab** | 切换窗口（透传，切换器不被抑制） |
| **Esc** | 关闭概览 |

## 架构说明

详见 [WINOME/README.md](WINOME/README.md)。

## 权限要求

- 隐藏任务栏、操作 Explorer 窗口需要**管理员权限**运行 `winome.exe`
- 宿主无终端窗口；启动错误与诊断信息写入 `%TEMP%\winome.log`

## 许可

GPL-2.0，详见 `LICENSE` 与 `WINOME/THIRD-PARTY-NOTICES`。
