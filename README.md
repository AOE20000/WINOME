# WINOME

<img width="1920" height="1080" alt="图片" src="https://github.com/user-attachments/assets/a38220d2-7f62-4b80-9da8-c73457b3b63c" />


在 Microsoft Windows 上用 Windows 原生功能支撑的 GNOME Shell UI 复刻。

## 仓库结构

```
.
├── WINOME/          # 主工程（meson 构建根，C++ 宿主 + GTK4 UI + St 引擎）
├── docs/            # 项目开发文档（环境、架构、主题管线、踩坑记录与开发日志）
├── extracted/       # 从上游提取、经 WINOME 修改的参考模块（去掉了上游 git）
│   ├── GMONE-Shell/ # Shell 外观（Sass 主题 + SVG）+ 布局/日期/动画算法参考
│   └── WinOverview/ # 旧概览方案（DLL 注入，已退役，仅参考）
├── upstream/        # 上游完整仓库（独立 git，仅供参考/更新提取）
│   ├── gnome-shell/ # GNOME Shell（GPL-2.0-or-later）
│   ├── Seelen-UI/   # 启发实现（AGPL-3.0-or-later，仅致谢，不派生）
│   └── WinOverview/ # GNOME 概览复刻（GPL-2.0）
├── .github/         # GitHub Actions CI 工作流（UCRT64 测试、release 构建与打包）
├── WINOME/package.sh# 自包含便携版打包脚本（收集运行时 DLL、loaders、schemas 与图标）
├── build.sh         # 交互式构建程序（生产/测试/调试方案）
└── LICENSE / README.md
```

- **宿主进程**：`WINOME/src/`（C++17 + GTK4，单进程含配置、i18n、设置窗、系统托盘与多屏管理）
- **概览（Activities）**：`WINOME/src/overview*.cpp` + `app-index.cpp` + `app-grid.cpp`（搜索、分页应用网格、Dash Favorites、DWM 缩略图与跨桌面拖拽）
- **St CSS 引擎复刻**：`WINOME/st-compat/`（libcroco + st-theme-node，去 Clutter/Cogl）
- **隐藏/替换 Windows 原生界面**：`WINOME/src/`（`native-taskbar.cpp` + `win-event-hook.cpp`，独立实现，思路受 Seelen-UI 启发）
- **虚拟桌面**：`WINOME/src/virtual-desktop.cpp`（移植并扩展 VirtualDesktopAccessor-rust COM 接口，覆盖 Win11 22621 / 24H2+）
- **系统集成与通知**：`WINOME/src/winrt-bridge.cpp`、`notifications.cpp`、`systray.cpp`、`monitor-manager.cpp`
- **外观与布局算法参考**：`extracted/GMONE-Shell/`

## 构建（MSYS2 UCRT64）

推荐使用根目录的交互式构建程序（环境配置与已知构建问题见 [docs/build-and-env.md](docs/build-and-env.md)）：

```bash
./build.sh          # 交互式菜单
./build.sh release  # 或直接指定方案
```

| 方案 | 配置 | 构建目录 | 说明 |
|------|------|----------|------|
| `release` 生产 | release + LTO + NDEBUG | `WINOME/build-release/` | 最小体积、最快指令（约 1.9MB） |
| `test` 测试 | debugoptimized | `WINOME/build/` | 项目默认，保留 `g_print` 诊断日志 |
| `debug` 调试 | debug（-O0 -g） | `WINOME/build-debug/` | GDB 友好 |

其他子命令：`./build.sh run`（运行最近构建）、`./build.sh clean`（清理全部构建目录）。三方案独立目录，切换互不触发重编。

### 测试、样式检查与便携打包

```bash
# 运行单元测试与 CSS 转换测试
meson test -C WINOME/build --print-errorlogs

# 检查 C++ 源码中使用的样式类覆盖率
python3 WINOME/tools/check-widget-classes.py WINOME/build/src/theme/gnome-shell-gtk4.css WINOME/src/*.cpp

# 生成自包含便携目录与 zip 安装包（默认先执行 ./build.sh release，可传 --no-build 复用）
./WINOME/package.sh
```

也可手动构建（等价于测试方案，注意先设置 ASCII 临时目录，详见 [docs/build-and-env.md](docs/build-and-env.md)）：

```bash
cd WINOME
meson setup build
ninja -C build
./build/src/winome.exe
```

## 当前状态

- [x] 宿主进程骨架 + 置顶全宽顶栏（Activities 圆点 + 时钟 + 状态区图标 + 悬停/点击动画）
- [x] 崩溃自愈与单实例 IPC：独立 `--watchdog` 看门狗、`--restore` 急救、`MiniDumpWriteDump` 转储、命名互斥体及 `--show-overview` / `--settings` / `--reload` / `--quit` 控制指令
- [x] 隐藏原生任务栏、事件钩子防重现、系统托盘兼容层（`systray.cpp` 在顶栏状态区提供折叠/弹出式托盘图标与右键交互）
- [x] 概览触发接入（**Win 键**、**左上角热角**触发，`Win+Tab` 打开开始菜单，其余 Win 组合键透传，均受 `config.json` 开关控制）
- [x] 概览显示**最小化窗口**；面板与弹出窗位于概览之上；顶栏保留工作区；面板按显示器独立判断全屏自动隐藏；隐藏于 Alt-Tab（`WS_EX_TOOLWINDOW`）
- [x] 多显示器与事件驱动：多屏顶栏/工作区、`WM_DISPLAYCHANGE` / `WM_DPICHANGED` / `WM_SETTINGCHANGE` / `WTSRegisterSessionNotification` 响应，WASAPI 音量与 `WlanRegisterNotification` 事件驱动更新
- [x] 快速设置全量闭环：电量/电源模式/电源动作（含 `SE_SHUTDOWN_NAME`）、WASAPI 音量与静音、WMI/DXVA2 屏幕亮度、Wlan 枚举与加密网络密码输入连接、WinRT 蓝牙设备切换、深色主题/勿扰/夜间模式/飞行模式
- [x] 日期与通知中心：双栏 DateMenu（SMTC 媒体控制卡片 + Windows 通知列表 + 勿扰联动与日历）、实时桌面通知横幅
- [x] **Activities 概览完全体**：`Ctrl+F`/直输搜索（开始菜单 `.lnk`/AUMID 应用 + 打开窗口）、Dash 固定 Favorites 持久化/右键菜单与图标 LRU 缓存、宿主内分页应用网格、窗口中键关闭、拖拽窗口缩略图跨桌面移动及 `+`/`×` 新建/删除虚拟桌面
- [x] **Windows 虚拟桌面兼容**：基于 `RtlGetVersion` 区分 Win11 22621 / 24H2+ COM 接口槽位表（支持查询、切换、固定、移动窗口与创建/移除桌面）
- [x] 配置、多语言与原生地设置页：`%LOCALAPPDATA%\WINOME\config.json` 热重载、`i18n.cpp` 多语言、Adwaita-dark GTK4 设置窗口
- [x] 自动化测试、CI 与便携打包：`meson test` 单元测试、GitHub Actions CI（`.github/workflows/ci.yml`）及 `WINOME/package.sh` 自包含便携包

## 快捷键与交互

| 按键 / 操作 | 行为 |
|------|------|
| **Win**（单独按下松开） | 切换概览（Activities） |
| **鼠标移到左上角热角** | 打开概览 |
| **Win+Tab** | 打开开始菜单 |
| **Win+E / Win+R / Win+D …** | 透传给 Windows（组合键保留） |
| **Win+Alt+Tab** | 任务视图（透传） |
| **Win+Ctrl+←/→** | 切换虚拟桌面（透传；概览打开时自动跟随重排） |
| **Alt+Tab** | 切换窗口（透传，切换器不被抑制） |
| **Ctrl+F**（概览内） | 聚焦概览搜索框 |
| **Ctrl+Shift+N**（概览内） | 新建虚拟桌面 |
| **PageUp / PageDown**（概览内） | 应用网格翻页或切换上一个/下一个虚拟桌面 |
| **鼠标中键**（概览内） | 关闭窗口缩略图，或关闭悬停中的虚拟桌面药丸 |
| **拖拽窗口卡片到桌面药丸** | 将窗口移动到目标虚拟桌面（仅 1 个桌面时显示 `+` 新建区） |
| **Esc** | 依次清空搜索词 → 退出应用网格 → 关闭概览 |

## 文档导航

完整开发文档已统一收口至 [docs/README.md](docs/README.md)：

- 开发环境与构建排障：[docs/build-and-env.md](docs/build-and-env.md)
- 系统架构、概览与数据源：[docs/architecture.md](docs/architecture.md)
- 主题管线、St 引擎与 UI 规范：[docs/theme-and-ui.md](docs/theme-and-ui.md)
- 硬约束与踩坑备忘：[docs/pitfalls.md](docs/pitfalls.md)
- 生产级开发计划与路线图：[docs/roadmap.md](docs/roadmap.md)

## 权限与数据目录

- 隐藏任务栏、操作 Explorer 窗口需要**管理员权限**运行 `winome.exe`
- 宿主无终端窗口；日志（5MB 自动轮转 `winome.prev.log`）、配置文件 `config.json` 与崩溃转储 `crashes\*.dmp` 均位于 `%LOCALAPPDATA%\WINOME\`
- 如异常退出导致任务栏未恢复，可运行 `winome.exe --restore`（便携包内置 `restore-taskbar.bat`）立即还原任务栏与工作区

## 许可

GPL-2.0，详见 `LICENSE` 与 `WINOME/THIRD-PARTY-NOTICES`。
