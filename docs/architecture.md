# 系统架构与模块设计

WINOME 在宿主单进程 `winome.exe` 内以 C++17 + GTK4 渲染完整 Shell UI，并直接通过 Win32 / COM / DWM / Wlan / PowrProf API 对接 Windows 原生能力（无 DLL 注入）。

---

## 1. 宿主启动与退出流程（`src/main.cpp`）

1. `setup_log_file()` — 将 stdout / stderr 追加重定向到 `%TEMP%\winome.log`（GUI 子系统 `win_subsystem: 'windows'`，不弹窗）。
2. `winome::set_process_dpi_aware()` — 启用 Per-Monitor DPI Awareness v2（`src/host-utils.cpp`）。
3. `winome::enable_privilege(L"SeTcbPrivilege")` — 尝试提权（隐藏任务栏需要）。
4. `winome::NativeTaskbar::hide()` — `SHAppBarMessage(ABM_SETSTATE)` + `ShowWindowAsync(SW_HIDE)`，并启动事件驱动看守线程应对 Explorer 重建（`src/native-taskbar.cpp`）。
5. `winome::start_win_event_hook()` — 注册 `SetWinEventHook`（`EVENT_OBJECT_CREATE / SHOW / UNCLOAKED`）：当任务栏、通知中心或非全屏 XAML 浮窗重现时再次隐藏；**全屏 XAML island（Alt-Tab / 任务视图切换器）放行**（`src/win-event-hook.cpp`）。
6. `winome::start_overview_trigger()` — `WH_KEYBOARD_LL` 低层钩子：裸 `Win` 切换概览、`Win+Tab` 打开开始菜单、其余 `Win` 组合键与 `Alt+Tab` 透传、`Esc` 关闭概览（`src/overview-trigger.cpp`）。
7. 创建并定位 GTK4 顶栏面板（`src/shell-panel.cpp`）：
   - `SetWindowPos(HWND_TOPMOST)` 置顶到**主显示器原点**；
   - `WS_EX_TOOLWINDOW` 使其隐藏于 Alt-Tab 与任务栏；
   - `SPI_SETWORKAREA` 保留主显示器顶部面板条带，使最大化窗口停在面板下方；
   - 调用 `overview_init(hwnd)`、`start_fullscreen_watcher(hwnd)`、`start_hot_corner(hwnd)`。
8. 延迟非关键路径初始化：
   - 虚拟桌面 COM 服务延后至启动后 `2s` / `3s`（避开低完整性下 explorer RPC 死锁的 8s 超时阻塞启动）；
   - `g_timeout_add(3000, pin_shell_windows, ...)` + `EVENT_OBJECT_SHOW` 钩子将本进程所有可见顶层窗口固定到全部虚拟桌面。
9. 退出时（`on_shutdown`）：撤销事件钩子、恢复 `SPI_SETWORKAREA` 并调用 `NativeTaskbar::restore()`（仅当本进程隐藏过才恢复）。

---

## 2. 快捷键与热区触发

### 键盘钩子（`src/overview-trigger.cpp`）

- 吞掉单独的 `Win` `WM_KEYDOWN`，阻止系统直接弹出开始菜单；若在 `WM_KEYUP` 判定为裸 `Win` 按下松开，则切换概览。
- 若期间检测到与其他键组合，重新注入带 `dwExtraInfo` 标记的 `Win` 键，让系统照常处理 `Win+E`、`Win+R`、`Win+D`、`Win+Ctrl+←/→`、`Win+Alt+Tab` 等组合键。
- `Win+Tab` 通过 `SendInput` 合成带标记的裸 `Win` 键打开开始菜单。
- `Esc` 在概览激活时关闭概览。

### 左上角热角（`src/hot-corner.cpp`）

每 `100ms` 轮询鼠标坐标，进入主显示器左上角热角时打开概览（带 `2s` 冷却；面板全屏隐藏时热角不触发）。

---

## 3. 顶部面板与弹出窗（`src/shell-panel.cpp`）

- **布局移植**：自定义 `WinomePanelLayout` 复刻 `panel.js` `vfunc_allocate()`，保证中间时钟相对显示器工作区严格居中，左右侧盒钳制在各自自然宽度（避免 `GtkCenterBox` 在剩余空间居中导致时钟偏移）。
- **图层与置顶**：面板与弹出窗各自作为独立无边框顶层窗口（`shadow-wrap` 卡片），由 `WH_MOUSE_LL` 钩子处理外部点击关闭；`WS_EX_TOOLWINDOW` 由 `fullscreen-watcher` 周期性重断言防止被 GDK configure 重置。
- **层级契约**：`popover > panel > 概览 chrome 窗口 > 概览窗口 > 应用 > 壁纸`。
- **与概览协作**：
  - 打开概览时关闭已开弹出窗，并给面板加 `.overview` 类使底色透明（`#panel.overview { background-color: transparent }`，对应上游 `#panel:overview`）；
  - 允许在概览上方直接打开快速设置或日历；
  - 弹出窗被外部点击关闭时，通过 `winome_shell_panel_popover_just_closed()` 通知概览吞掉该次点击（对应 GNOME 菜单 grab 语义），不连带关闭概览。

---

## 4. 原生 Activities 概览（`src/overview*.cpp`）

已全面替代早期基于 DLL 注入的 `extracted/WinOverview/` 方案，在宿主进程内基于 `overviewControls.js`（`WINDOW_PICKER` 状态）与 `workspace.js` 逐组件移植。

### 布局与交互模块

| 模块 | 上游参考 | 职责 |
|------|----------|------|
| `overview.cpp` | `overviewControls.js`, `windowPreview.js` | 搜索框占位（24em）、工作区背景盒（按工作区纵横比居中 + cover 壁纸 + 0→30px 圆角插值）、DWM 实时缩略图注册与飞入/开合动画、悬停 chrome 透明窗 |
| `overview-layout.cpp` | `workspace.js` (`UnalignedLayoutStrategy`) | `lerp(1.5, 1)` 窗口缩放、行搜索最优 scale/space、`WINDOW_PREVIEW_MAXIMUM_SCALE = 0.95`、像素对齐、有效间距（主题 6px + chrome oversize） |
| `overview-dash.cpp` | `dash.js` | 按 AppUserModelID / 进程归组的运行应用图标、运行指示点、“显示应用”入口、图标大小自动步降（16..64）与 300ms 悬停标签 |
| `overview-thumbs.cpp` | `workspaceThumbnail.js` | >1 个虚拟桌面时显示壁纸药丸行（4px 圆角 + 活动桌面 3px `#3584e4` 指示器，上限为工作区高度 5%），药丸内带各桌面窗口的 DWM 迷你预览；点击切换桌面且概览保持打开 |
| `ws-osd.cpp` | `workspaceSwitcherPopup.js` | 概览外 `Win+Ctrl+←/→` 切换桌面时，主屏底部中央（4em 边距）显示圆点 OSD（100ms EASE_OUT_QUAD 淡入 → 600ms 常驻 → 100ms 淡出，连续切换重置计时） |

### 关键实现细节

- **开合动画**：`250ms`（开 `EASE_OUT_SINE` / 关 `EASE_OUT_QUAD`）；窗口以 `gtk_window_fullscreen` 创建保持 surface 与显示器一致；布局 idle 使用 `G_PRIORITY_HIGH_IDLE` 抢在首帧重绘前就绪，避免黑闪；关闭后保留控件几何以便重开首帧铺满壁纸。
- **最小化窗口**：使用 `GetWindowPlacement().rcNormalPosition` 作为原矩形并按恢复矩形归属显示器，避免 `GetWindowRect` 返回离屏坐标；开合动画从工作区角落展开并淡入；点击激活时先 `SW_RESTORE` 再 `SetForegroundWindow`。
- **排除自身窗口**：枚举窗口时用 `is_our_process()` 过滤本进程所有窗口（面板、弹出窗、概览与 chrome 窗口）。
- **悬停 chrome 独立顶层窗**：因为 DWM 缩略图合成在窗口自绘内容之上，关闭按钮（32px）与应用图标（64px，`WM_GETICON` / `IShellItemImageFactory`）置于独立透明顶层窗（`WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`）浮于缩略图之上，指针离开 `750ms` 后淡出；关闭按钮发送 `WM_CLOSE` 并触发重排。
- **工作区切换动画**（`switch_workspace_animated`）：概览内切换桌面时，旧桌面缩略图移入离场组，与新桌面同步横向滑动（`250ms EASE_OUT_CUBIC`），背景盒随行平移，缩略图指示器连续插值滑动；外部切换由 400ms 轮询跟随重排。

---

## 5. 虚拟桌面 COM 模块（`src/virtual-desktop.cpp`）

移植自 [VirtualDesktopAccessor-rust](https://github.com/Ciantic/VirtualDesktopAccessor)（MIT），在 Windows 11 22621 / 24H2 验证：

- **接口获取**：`CoIncrementMTAUsage` → `CLSID_ImmersiveShell` → `QueryService` 获取 `IVirtualDesktopManagerInternal`（枚举/切换）与 `IVirtualDesktopManager`（窗口归属）。
- **22621 / 24H2 槽位兼容**：两者接口 IID 相同（`53F5CA0B`），但 24H2 在 slot 10 插入了 `switch_desktop_and_move_foreground_view`，导致其后所有槽位 `+1`。WINOME 仅调用偏移区之前的稳定槽位（`GetDesktops` / `GetCurrentDesktop` / `SwitchDesktop`），切换通过 `GetAt(index)` 解析，绝不调用 `find_desktop`。
- **调用顺序**：先枚举再查当前桌面（22621 上直接先调 `GetCurrentDesktop` 会导致 explorer 服务挂起）。
- **防 GCC 去虚拟化**：所有 COM 调用通过裸 vtable 函数指针发起，避免 GCC `-O2` 将纯虚接口去虚拟化为 `__cxa_pure_virtual` 导致运行时跳到非法地址崩溃。
- **专用工作线程 + 超时自保护**：全部 VD COM 调用封送至单工作线程顺序执行，主线程等待完成并带 `8s` 超时；超时（如非提权/低完整性环境 explorer RPC 死锁）后永久禁用 VD 模块，概览平稳降级为单工作区布局。
- **跨桌面固定（`pin_window`）**：通过 `IVirtualDesktopPinnedApps`（IID `4CE81583`）与 `IApplicationViewCollection`（IID `1841C6D7`）将壳层全部窗口固定到所有桌面；若 pin 服务不可达仅关闭 pin，不影响其余 VD 功能。

---

## 6. Windows 原生数据源

| 模块 | 实现机制 | 备注与降级策略 |
|------|----------|----------------|
| **Wi-Fi**（`src/wifi-networks.cpp`） | 运行时 `LoadLibraryW("wlanapi.dll")` 动态解析 7 个 Wlan API，进程级常驻 `WlanOpenHandle` | 无无线网卡或缺失 `wlanapi.dll` 时安静降级（隐藏状态指示器与菜单箭头）；按 BSSID 合并同名 SSID 保留最强信号；`copy_ssid` 先校验 UTF-8 失败再按 Latin-1 展开（97 字节缓冲，含 NUL 判为隐藏网络）；点击使用已存 profile `WlanConnect` |
| **电源模式**（`src/power-modes.cpp`） | `PowrProf` + `powersetting.h` 枚举并切换节能（`a1841308…`）/ 均衡（`381b4222…` 与 Win11 随机化 GUID `b9a9a03a…`）/ 性能（`8c5e7fda…`） | `PowerEnumerate` 探测缺失的方案不显示；无法识别时副标题回退显示均衡 |
| **系统状态**（`src/system-status.cpp`） | 电量 `GetSystemPowerStatus`、音量 WASAPI `IAudioEndpointVolume`、网络回退 `InternetGetConnectedState`、锁屏 `LockWorkStation`、挂起 `SetSuspendState`、注销/重启/关机 `ExitWindowsEx` | 屏幕亮度 `get_brightness()` 当前为占位 `0.5`，`set_brightness()` 待接入 WMI / DXVA2 |

---

## 7. 性能与权限基线

### 性能优化措施

- **动画走帧时钟**：概览开合、悬停 chrome、菜单开合均使用 `gtk_widget_add_tick_callback`（对齐显示器 vblank），空闲零开销。
- **z-order 漂移门控**：`fullscreen-watcher` 每 `400ms` 先用 `GetWindow(GW_HWNDPREV)` 校验 `popover > panel > chrome > overview` 链，链路完整则直接跳过 `SetWindowPos`，消除无意义的 `WM_NCCALCSIZE` 往返。
- **重绘守卫**：状态图标使用 `set_icon_name_if_changed` 避免相同图标名触发重绘节点失效；悬停标题药丸在纯淡入/淡出帧跳过 `gtk_fixed_move`。
- **GPU 渲染与内存基线**：
  - 关键窗口由 OpenGL（`GskGLRenderer` / `ngl`）硬件绘制，`[gpu]` 启动日志记录渲染器与 GL 厂商；
  - **开发构建（`test`）**：约 `450MB`（Pango fontmap 枚举全系统约 290 族字体并在约 6s 后稳定驻留，属正常内存数据库占用而非泄漏）；
  - **生产构建（`release` + LTO）**：约 `30MB`。

### 权限要求

- 隐藏原生任务栏、操作 Explorer 窗口及完整使用虚拟桌面 COM 服务需要以**管理员权限**运行 `winome.exe`。
- 未提权运行时，若 Explorer RPC 死锁，VD 模块将在 8s 超时后自动禁用并回退单工作区，保证 UI 不崩溃、不冻结。
