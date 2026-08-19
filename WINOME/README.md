# WINOME

在 Microsoft Windows 上用 Windows 原生功能支撑的 GNOME Shell UI 复刻。

- **宿主进程**：C++（`src/`），GTK4 UI
- **隐藏/替换 Windows 原生界面**：`src/native-taskbar.cpp` + `src/win-event-hook.cpp`（独立实现）
- **概览（Activities）**：`src/overview.cpp` 宿主进程内 GTK4 渲染 + DWM 缩略图（旧 DLL 注入方案已退役）
- **虚拟桌面**：`src/virtual-desktop.cpp`（移植 VirtualDesktopAccessor-rust 的 COM 接口）
- **外观与布局算法参考**：`../extracted/GMONE-Shell/`（Sass 主题 + JS 算法）
- **St CSS 引擎**：`st-compat/`（libcroco + st-theme-node，运行时查询 GNOME 样式）

## 构建（MSYS2 UCRT64）

```bash
meson setup build
ninja -C build
./build/src/winome.exe   # 需管理员权限运行
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
- [x] **原生概览重写**（`src/overview.cpp`，替代旧 DLL 注入）：overviewControls.js WINDOW_PICKER 布局逐行移植——搜索栏（24em 药丸）、工作区背景（保持工作区纵横比的居中盒子 + cover 壁纸 + 0→30px 圆角插值）、窗口网格（`UnalignedLayoutStrategy`，含 chrome oversize 有效间距）、dash、悬停 chrome（预览放大 + 64px 应用图标 + 标题胶囊 + 关闭按钮，750ms 空闲隐藏）
- [x] 概览开合动画：`Overview.ANIMATION_TIME` 250ms（开 EASE_OUT_SINE / 关 EASE_OUT_QUAD）；窗口从真实位置飞入槽位，最小化窗口从角落展开 + 淡入；布局 idle 用 `G_PRIORITY_HIGH_IDLE` 抢在重绘前执行，杜绝首帧黑闪
- [x] **Windows 虚拟桌面兼容**（`src/virtual-desktop.cpp`，移植 VirtualDesktopAccessor-rust）：Win11 22621/24H2 COM 接口（ImmersiveShell → IVirtualDesktopManagerInternal），枚举/切换桌面、窗口归属查询
- [x] **工作区缩略图**（`src/overview-thumbs.cpp`，workspaceThumbnail.js 移植）：>1 桌面时显示壁纸药丸行（4px 圆角 + 活动桌面 3px accent 指示器），药丸内含各桌面窗口的 DWM 迷你预览；点击切换桌面（概览保持打开），Win+Ctrl+←/→ 外部切换时 400ms 轮询跟随重排
- [x] 概览内可打开快速设置/日历（弹出窗浮于概览之上，关闭弹窗的外部点击不再连带关闭概览——对应 GNOME 菜单 grab 吞点击）
- [ ] 二级菜单（WLAN 网络/蓝牙设备/电源模式/关机）、日历面板重构、网络/蓝牙真实数据
- [ ] 概览搜索（应用/窗口检索）

## 快捷键

| 按键 | 行为 |
|------|------|
| **Win**（单独按下松开） | 切换概览 |
| **鼠标移到左上角热角** | 打开概览（`src/hot-corner.cpp`，主显示器左上角，带 2s 冷却） |
| **Win+Tab** | 打开开始菜单 |
| **Win+E / Win+R / Win+D …** | 透传给 Windows（组合键保留） |
| **Win+Alt+Tab** | 任务视图（透传） |
| **Win+Ctrl+←/→** | 切换虚拟桌面（透传；概览打开时自动跟随重排） |
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
6. `start_overview_trigger()` — `WH_KEYBOARD_LL` 钩子：裸 **Win** 切换概览、Win+Tab 打开开始菜单、其余 Win 组合键/Alt+Tab 透传（概览在本进程内 GTK 渲染，详见下节）；Esc 关闭
7. 创建 GTK4 面板窗口（置顶全宽顶栏，时钟/Activities/快速设置）：`SetWindowPos(HWND_TOPMOST)` 定位到**主显示器原点**；`WS_EX_TOOLWINDOW` 使其隐藏于 Alt-Tab/任务栏；`SPI_SETWORKAREA` 保留顶栏条带（应用停在面板下方）；`start_fullscreen_watcher()` 每 400ms 检查前台窗口（真全屏/锁屏隐藏面板，同时重断言 `WS_EX_TOOLWINDOW` 并重排概览层级）+ 轮询虚拟桌面切换；`start_hot_corner()` 每 100ms 轮询光标，进入左上角热角打开概览
8. 退出时恢复工作区与 `NativeTaskbar::restore()` — 仅当本进程隐藏过才恢复

构建产物（`build/` 下）：
- `src/winome.exe` — 宿主进程（概览同进程渲染，无 DLL/注入）

### 概览（`src/overview.cpp`，宿主进程内）

对 gnome-shell `js/ui/overviewControls.js` WINDOW_PICKER 状态的逐组件移植：

- **布局**（`ControlsManagerLayout` + `WorkspacesView`）：搜索栏居中（24em）；工作区缩略图行（>1 桌面时，上限为工作区高度 5%）；工作区盒与工作区同纵横比、居中（`_getFirstFitSingleWorkspaceBox`）；dash 底部居中（16% 高度上限）；z 序按 `ControlsManager.add_child`——背景在 dash/搜索栏之上（开合动画中它们从背景边缘后浮现）
- **窗口网格**（`overview-layout.cpp`，`UnalignedLayoutStrategy` 1:1 移植）：`lerp(1.5,1)` 窗口缩放、行搜索最优 scale/space、`WINDOW_PREVIEW_MAXIMUM_SCALE` 0.95、像素对齐；有效间距 = 主题 6px + windowPreview chrome oversize（关闭按钮半高/图标外沿 + 5px）
- **DWM 缩略图**：`DwmRegisterThumbnail` 挂在概览顶层窗口上（要求顶层目标），飞入动画 = 逐帧 `DWM_TNP_RECTDESTINATION` 插值；挂起的 UWP（cloaked）窗口按桌面归属过滤后同样渲染
- **悬停 chrome**（windowPreview.js）：预览中心放大 10px（200ms EASE_OUT_QUAD）；64px 应用图标（WM_GETICON / IShellItemImageFactory，独立透明窗口承载于缩略图之上）、标题胶囊、右上 32px 关闭按钮；离开 750ms 后淡出；关闭按钮发 `WM_CLOSE` 后重排
- **开合动画**：250ms（开 EASE_OUT_SINE / 关 EASE_OUT_QUAD）；背景在全工作区（圆角 0）与工作区盒（圆角 30）间插值，窗口反向飞回原位，缩略图行淡入淡出；窗口 `gtk_window_fullscreen` 创建（surface 尺寸即显示器，无 resize 增长）；关闭后保留控件几何使重开首帧即壁纸铺满工作区
- **层级契约**：popover > panel > 概览 chrome 窗口 > 概览窗口 > 应用 > 壁纸，由 fullscreen-watcher 周期重断言；概览窗口 `WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST`
- **概览内弹出窗**：快速设置/日历可直接在概览上方打开（GNOME 原生行为）；关闭弹窗的外部点击被吞并（菜单 grab 语义），不会连带关闭概览

### 虚拟桌面（`src/virtual-desktop.cpp`）

移植 [VirtualDesktopAccessor-rust](https://github.com/Ciantic/VirtualDesktopAccessor)（MIT）的 COM 接口定义，Win11 22621/24H2 验证：

- `CoIncrementMTAUsage`（MTA 常驻）→ `CLSID_ImmersiveShell` → `QueryService` 取 `IVirtualDesktopManagerInternal`（枚举/切换）+ `IVirtualDesktopManager`（窗口归属）
- **布局兼容 22621 与 24H2**：接口 IID 相同（53F5CA0B），但 24H2 在 slot 10 插入了 `switch_desktop_and_move_foreground_view`，其后所有槽位 +1（如 `find_desktop` 22621=slot 13 / 24H2=slot 14）。winome 只调用偏移区之前的稳定槽位：`GetDesktops`/`GetCurrentDesktop`/`SwitchDesktop`——桌面切换经 `GetAt(index)` 解析，绝不使用 `find_desktop`
- **调用顺序**：先枚举再查当前桌面（22621 上直接先调 `GetCurrentDesktop` 会使 explorer 服务挂起）
- **所有 COM 调用走裸 vtable**（运行时函数指针）：GCC -O2 会对全纯虚接口结构做去虚拟化（本翻译单元无派生类），把调用替换成 `__cxa_pure_virtual`，而 MinGW 链接器把该弱符号解析到垃圾地址 `0x100000000`——运行时跳转到镜像基址下方 256MB（某 DLL 基址）即 SIGSEGV。裸调用无法被去虚拟化
- **专用 COM 线程**：所有调用 marshalled 到单个工作线程（上游注释："VD COM Objects don't like being called from different threads rapidly"）；主线程等待**完成**（非取走）并带 8s 超时——explorer 端死锁（如低完整性调用方）时放弃并永久禁用模块，概览回退单工作区布局，绝不冻结 UI
- 不可用环境（旧版本 Windows / explorer 死锁）下安静降级：`desktops()` 返回空，概览布局与单工作区完全一致

### 工作区缩略图（`src/overview-thumbs.cpp`）

`workspaceThumbnail.js` ThumbnailsBox 的视觉移植：每桌面一枚 4px 圆角壁纸药丸（cover 渲染），活动桌面套 3px accent（#3584e4）8px 圆角指示器；缩放 `min(hScale, vScale, 0.05)`；主题值（spacing/padding 6px、#46464e）经 St 引擎查询。药丸内的窗口迷你预览 = 窗口矩形裁剪到工作区后按药丸缩放系数放置的 DWM 缩略图。点击药丸 → `SwitchDesktop` → 概览原地重排（概览保持打开，GNOME 行为）。

### 概览与面板的协作（本进程实现）

- **排除 WINOME 自身窗口**：概览枚举用 `is_our_process()` 过滤本进程窗口（面板/弹出窗/概览 chrome 不进缩略图）；旧 DLL 方案的 `IsWinomeHostWindow` 跨进程标题匹配已随 DLL 退役
- **最小化窗口**：`GetWindowPlacement().rcNormalPosition` 作为矩形（`GetWindowRect` 对最小化返回离屏坐标）、按恢复矩形归属显示器；打开动画从工作区角落展开 + 淡入（对应 `showing_on_its_workspace() == false` 分支）；点击时先 `SW_RESTORE` 再 `SetForegroundWindow`
- **顶层缩略图**：DWM 要求目标是顶层窗口，概览本体即顶层窗口，缩略图天然与概览同级渲染；关闭按钮/应用图标等交互件放在独立的 chrome 透明窗口（`WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST`）浮于缩略图之上

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
- 虚拟桌面 COM 接口对低完整性调用方可能死锁（explorer 端 RPC 不响应）：未提权运行时 VD 模块在 8s 超时后自动禁用，概览回退单工作区布局（不崩溃、不冻结）；**提权运行则完整可用**
- `SeTcbPrivilege` 由 `enable_privilege` 尝试启用

## 许可

GPL-2.0，详见 `LICENSE` 与 `THIRD-PARTY-NOTICES`。
