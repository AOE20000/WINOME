# WINOME

在 Microsoft Windows 上用 Windows 原生功能支撑的 GNOME Shell UI 复刻。

- **宿主进程**：C++（`src/`），GTK4 UI
- **隐藏/替换 Windows 原生界面**：`src/native-taskbar.cpp` + `src/win-event-hook.cpp`（独立实现）
- **概览（Activities）**：`../extracted/WinOverview/`（C++ DLL 注入 + DWM 缩略图）
- **外观与布局算法参考**：`../extracted/GMONE-Shell/`（Sass 主题 + JS 算法）
- **St CSS 引擎**：`st-compat/`（libcroco + st-theme-node，运行时查询 GNOME 样式）

## 构建（MSYS2 UCRT64）

```bash
meson setup build
ninja -C build
./build/src/winome.exe
```

## 当前状态

- [x] Phase 0：宿主进程骨架 + 顶部面板占位（GTK4），可编译运行
- [x] 隐藏原生任务栏（`src/native-taskbar.cpp`，独立实现）
- [x] 桌面层嵌入 WorkerW（`src/desktop-layer.cpp`，独立实现）
- [x] 事件钩子防止任务栏/通知中心/XAML 浮窗重现（`src/win-event-hook.cpp`；**保留全屏 Alt-Tab/任务视图切换器**）
- [x] 接入 WinOverview 概览（**Win 键** / **左上角热角**触发，`src/overview-trigger.cpp` + `src/hot-corner.cpp` + `overview/` 编译 DLL/Launcher）
- [x] 概览增强：显示**最小化窗口**（恢复矩形、无离屏飞入）、排除 WINOME 面板、让出顶栏条带（面板保持可见）
- [x] 面板：置顶全宽顶栏，GNOME 主题精确复刻（Activities 圆点按钮 + 时钟 + 状态区图标 + 悬停/点击动画）
- [x] 面板保留工作区（`SPI_SETWORKAREA`）：最大化/全屏应用停在面板下方（像任务栏保留底部）
- [x] 面板全屏自动隐藏（`src/fullscreen-watcher.cpp`）：真全屏/锁屏隐藏，桌面/最大化/概览/Alt-Tab 切换器下保持显示
- [x] 面板隐藏于 Alt-Tab 与任务栏（`WS_EX_TOOLWINDOW`，GDK 重置后周期性重断言）
- [x] 宿主为 GUI 程序（`win_subsystem: 'windows'`），stdout/stderr 写入 `%TEMP%\winome.log`
- [x] 快速设置面板：GNOME 完整布局——电量+操作按钮顶行、音量/亮度滑块（整行，accent 进度 + 白色圆手柄）、8 个开关（WLAN/蓝牙/电源模式/夜间模式/深色主题/勿扰/键盘/飞行模式，菜单型开关带 `>` 分隔 + 二级箭头，accent 选中态）
- [x] 真实数据源：电量（`GetSystemPowerStatus`）、音量（WASAPI `IAudioEndpointVolume`，滑块实时调节 + 图标动态）、网络（`InternetGetConnectedState`）、锁屏（`LockWorkStation`）
- [x] 时钟 UTF-8 修复（改用 `GDateTime`，避免 strftime 的 GBK `%p` 触发 Pango 报错）
- [x] 快捷键：Win = 概览、**左上角热角** = 打开概览、Win+Tab = 开始菜单、其余 Win 组合键/Alt+Tab 透传、Esc = 关闭概览
- [x] 快速设置/日历弹出窗重新接入（点击状态区打开快速设置、点击时钟打开日历）：自绘无边框顶层窗口，右侧对齐/时钟居中、面板下方 8px + 屏幕侧边 12px 稳定间隙；WH_MOUSE_LL 钩子处理外部点击关闭；打开时置顶（概览上方）；概览打开时自动关闭弹出窗、面板/弹出窗上的点击不转发给概览
- [x] 概览层级集成：概览打开时面板加 `.overview` 类使顶栏底色透明（`#panel.overview { background-color: transparent }`，对应原生 `#panel:overview`），顶栏与弹出窗浮于概览之上且可交互
- [ ] 二级菜单（WLAN 网络/蓝牙设备/电源模式/关机）、日历面板重构、网络/蓝牙真实数据
- [ ] 网络/蓝牙真实数据

## 快捷键

| 按键 | 行为 |
|------|------|
| **Win**（单独按下松开） | 切换概览 |
| **鼠标移到左上角热角** | 打开概览（`src/hot-corner.cpp`，主显示器左上角，带 2s 冷却） |
| **Win+Tab** | 打开开始菜单 |
| **Win+E / Win+R / Win+D …** | 透传给 Windows（组合键保留） |
| **Win+Alt+Tab** | 任务视图（透传） |
| **Alt+Tab** | 切换窗口（透传，切换器不被抑制） |
| **Esc** | 关闭概览 |

> Win 键实现：钩子吞掉 Win keydown 阻止系统打开开始菜单；裸 Win 在 keyup 判定后切换概览；检测到组合键时重新注入带标记的 Win（`dwExtraInfo`），系统照常处理 Win+E 等；Win+Tab 用 `SendInput` 合成裸 Win 打开开始菜单。

## 架构说明

宿主进程（`winome.exe`）启动时：

1. `setup_log_file()` — 重定向 stdout/stderr 到 `%TEMP%\winome.log`（GUI 程序，无终端窗口）
2. `set_process_dpi_aware()` — 每显示器 DPI 感知 v2
3. `enable_privilege(SeTcbPrivilege)` — 提权（隐藏任务栏需要）
4. `NativeTaskbar::hide()` — `SHAppBarMessage(ABM_SETSTATE)` + `ShowWindowAsync(SW_HIDE)`，事件驱动看守线程应对 Explorer 重建
5. `start_win_event_hook()` — `SetWinEventHook` 分别注册 `EVENT_OBJECT_CREATE/SHOW/UNCLOAKED`：任务栏/通知中心/非全屏 XAML 浮窗重现时再次隐藏；**全屏 XAML island（Alt-Tab/任务视图切换器）放行**
6. `start_overview_trigger()` — `WH_KEYBOARD_LL` 钩子：裸 **Win** 切换概览、Win+Tab 打开开始菜单、其余 Win 组合键/Alt+Tab 透传；在 explorer.exe 内 `WinExec` 启动 `WinOverviewLauncher.exe`（普通权限），由 launcher 拉起 RuntimeBroker 并注入 `WinOverview.dll` 渲染概览；Esc 关闭
7. 创建 GTK4 面板窗口（置顶全宽顶栏，时钟/Activities/快速设置）：`SetWindowPos(HWND_TOPMOST)` 定位到**主显示器原点**；`WS_EX_TOOLWINDOW` 使其隐藏于 Alt-Tab/任务栏；`SPI_SETWORKAREA` 保留顶栏条带（应用停在面板下方）；`start_fullscreen_watcher()` 每 400ms 检查前台窗口（真全屏/锁屏隐藏面板，同时重断言 `WS_EX_TOOLWINDOW`）；`start_hot_corner()` 每 100ms 轮询光标，进入左上角热角打开概览
8. 退出时恢复工作区与 `NativeTaskbar::restore()` — 仅当本进程隐藏过才恢复

构建产物（`build/` 下）：
- `src/winome.exe` — 宿主进程
- `overview/WinOverview.dll` — 概览渲染 DLL（注入 RuntimeBroker）
- `overview/WinOverviewLauncher.exe` — 注入启动器

### 概览与面板的协作（`extracted/WinOverview/` 的 WINOME 修改）

- **概览让出顶栏条带**：概览窗按标题 `WINOME Shell` 找到面板，若面板可见则其所在显示器概览窗起点下移一个面板高度（`realArea.top += panelHeight`），面板自然露出在上方。面板隐藏（全屏自动隐藏）时概览恢复全屏。
- **排除 WINOME 面板**：`EnumWindowsProc` 用 `IsWinomeHostWindow` 过滤——匹配标题 `WINOME Shell`（跨完整性可读）+ 进程名 `winome.exe` 兜底，面板/弹层不会成为缩略图。
- **最小化窗口**：去掉 `!IsIconic` 过滤；最小化窗口用 `GetWindowPlacement().rcNormalPosition` 作为矩形（`GetWindowRect` 对最小化返回离屏坐标）、按恢复矩形归属显示器；无离屏飞入动画（直接出现在槽位）；点击时先 `SW_RESTORE` 再 `SetForegroundWindow`。
- **DLL 运行时**：MinGW 运行时静态链接（`-static-libgcc -Bstatic -lstdc++ -lwinpthread`），DLL 只依赖系统 DLL——注入 RuntimeBroker 时其搜索路径（继承自 explorer）找不到 MSYS2 的 bin。
- **导出与关闭**：`overview_main` 用 `extern "C"` 导出（C++ 名称修饰会导致 launcher 的 `GetProcAddress` 返回 NULL）；关闭动画若与开场动画互斥则用 `SetTimer` 延迟重试，避免概览关不掉。

主题处理（`src/theme/meson.build` + `tools/` + `st-compat/`）：

**双 CSS 管线**：
1. `gnome-shell-dark.css`（sassc 编译 GNOME SCSS 的产物，提交在 `src/theme/`）——**原始 CSS，含 St 专属属性**
2. 它同时供两个消费者使用：
   - **St CSS 引擎**（`st-compat/`，libcroco + st-theme-node 复刻）：运行时解析原始 CSS，精确查询任意 St 属性（`icon-size`、`spacing`、`-natural-hpadding`、颜色、em 换算、级联继承），供 C++ 代码应用
   - **GTK4 渲染**（`tools/convert-st-css.py` → `gnome-shell-gtk4.css`）：把 St 专属属性映射/丢弃为 GTK4 等价（`height`→`min-height`、`-natural-hpadding`→`padding-left/right`），结果 0 个 GTK4 解析报错
3. GResource 同时打包两份 CSS：原始版给 St 引擎，转换版给 GTK4

### St CSS 引擎复刻（`st-compat/`）

完整抽取 GNOME 的 St 样式引擎（非补丁），让 WINOME 能**运行时精确查询** GNOME 主题属性，与旧版 GNOME 行为逐字节一致：

| 组件 | 来源 | 说明 |
|------|------|------|
| `croco/` | libcroco | CSS 解析器（21 个 .c，纯 GLib） |
| `st-theme.c` | gnome-shell | 样式表加载 + 级联 |
| `st-theme-node.c` | gnome-shell | 属性求值（4565 行核心，选择器匹配/em 换算/继承） |
| `st-shadow.c`、`st-border-image.c`、`st-icon-colors.c`、`st-types.h` | gnome-shell | 数据类型 |
| `st-compat.h` | WINOME | CoglColor/ClutterActorBox shim（去 Clutter/Cogl 依赖） |
| `st-compat-impl.c` | WINOME | StThemeContext/StSettings 极简实现 |
| `src/st-engine.{h,cpp}` | WINOME | C++ 绑定（StEngine 加载 + Node 查询，支持父链复合选择器） |

`extract-st-tokens.py`（旧的构建期正则提取）已删除，被 St 引擎运行时查询取代。

### St → GTK4 的差异处理（`convert-st-css.py`，仅用于 GTK4 渲染）

颜色求值（`st-mix()`/`st-lighten()`/`st-darken()`/`st-transparentize()`/`-st-accent-color`）**委托给 `st-css-eval` 工具**（链接 st-compat，复用 St 的 C 实现，结果与 GNOME 逐字节一致），而非在 Python 里复刻。

| GNOME (St) | GTK4 等价处理 |
|------------|--------------|
| `st-mix()/st-lighten()/st-darken()/st-transparentize()` | 委托 `st-css-eval`（St 原生颜色数学） |
| `-st-accent-color` / `-st-accent-fg-color` | 委托 `st-css-eval`（读 StThemeContext accent 色 #3584e4 / #fff） |
| `box-shadow: inset 0 0 0 100px <c>`（背景填充 hack） | 翻译成 `background-color: <c>`（GTK4 大 spread box-shadow 渲染有 bug） |
| `height`/`width` | `min-height`/`min-width`（GTK4 无 height/width） |
| `-natural-hpadding` | `padding-left` + `padding-right` |
| `:insensitive` 伪类 | `:disabled` |
| `:overview`/`:unlock-screen` 等状态伪类 | 整条规则丢弃（剥离会误应用到默认态） |
| `!important` | 剥离（GTK4 对 box-shadow 等不支持） |

### C++ 侧的关键适配（`shell-panel.cpp`）

- **widget 树复刻 GNOME**：Activities = `.panel-button` 按钮内含 `workspace-dot` 圆点；时钟 = `.panel-button.clock-display` 按钮内含 `.clock` label（高亮在子元素上）；状态区 = `.panel-button` 按钮内含 `.panel-status-indicators-box` + 图标。时钟/图标继承按钮的白色前景。
- **`gtk_widget_set_focus_on_click(FALSE)`**：GNOME 的 `:focus`/`:hover` 共用高亮，但 GTK4 点击会让按钮持续 `:focus` 导致高亮"粘住"，必须禁用点击聚焦。
- **St 属性运行时查询**：`status_icon_size()`/`status_indicators_spacing()` 通过 St 引擎按 `#panel .panel-button .system-status-icon` 等复合选择器查询 icon-size/spacing，替代硬编码。

## 权限要求

- 隐藏任务栏、操作 Explorer 窗口需要**管理员权限**运行 `winome.exe`
- `SeTcbPrivilege` 由 `enable_privilege` 尝试启用

## 许可

GPL-2.0，详见 `LICENSE` 与 `THIRD-PARTY-NOTICES`。
