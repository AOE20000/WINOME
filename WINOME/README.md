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
- [x] 事件钩子防止任务栏/通知中心/XAML 浮窗重现（`src/win-event-hook.cpp`，独立实现）
- [x] 接入 WinOverview 概览（Win+W 触发，`src/overview-trigger.cpp` + `overview/` 编译 DLL/Launcher）
- [x] 面板：置顶全宽顶栏，GNOME 主题精确复刻（Activities 圆点按钮 + 时钟 + 状态区图标 + 悬停/点击动画）
- [x] 快速设置面板：GNOME 完整布局——电量+操作按钮顶行、音量/亮度滑块（整行，accent 进度 + 白色圆手柄）、8 个开关（WLAN/蓝牙/电源模式/夜间模式/深色主题/勿扰/键盘/飞行模式，带 `>` 二级菜单箭头）
- [x] 真实数据源：电量（`GetSystemPowerStatus`）、音量（WASAPI `IAudioEndpointVolume`，滑块实时调节 + 图标动态）、网络（`InternetGetConnectedState`）、锁屏（`LockWorkStation`）
- [x] 时钟 UTF-8 修复（改用 `GDateTime`，避免 strftime 的 GBK `%p` 触发 Pango 报错）
- [ ] 日历面板完整实现（`dateMenu.js`）
- [ ] 关机菜单（挂起/重启/关机/注销）、亮度/网络/蓝牙真实数据
- [ ] 网络/蓝牙真实数据

## 架构说明

宿主进程（`winome.exe`）启动时：

1. `set_process_dpi_aware()` — 每显示器 DPI 感知 v2
2. `enable_privilege(SeTcbPrivilege)` — 提权（隐藏任务栏需要）
3. `NativeTaskbar::hide()` — `SHAppBarMessage(ABM_SETSTATE)` + `ShowWindowAsync(SW_HIDE)`，事件驱动看守线程应对 Explorer 重建
4. `start_win_event_hook()` — `SetWinEventHook` 分别注册 `EVENT_OBJECT_CREATE/SHOW/UNCLOAKED`，任务栏/通知中心/XAML 浮窗重现时再次隐藏
5. `start_overview_trigger()` — `WH_KEYBOARD_LL` 钩子监听 **Win+W**：在 explorer.exe 内 `WinExec` 启动 `WinOverviewLauncher.exe`（普通权限），由 launcher 拉起 RuntimeBroker 并注入 `WinOverview.dll` 渲染概览；Esc 关闭
6. 创建 GTK4 面板窗口（置顶全宽顶栏，时钟/Activities/快速设置），`SetWindowPos(HWND_TOPMOST)` 定位到工作区顶部
7. 退出时 `NativeTaskbar::restore()` — 仅当本进程隐藏过才恢复

构建产物（`build/` 下）：
- `src/winome.exe` — 宿主进程
- `overview/WinOverview.dll` — 概览渲染 DLL（注入 RuntimeBroker）
- `overview/WinOverviewLauncher.exe` — 注入启动器

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
