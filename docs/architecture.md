# 系统架构与模块设计

WINOME 在宿主单进程 `winome.exe` 内以 C++17 + GTK4 渲染完整 Shell UI，并直接通过 Win32 / COM / DWM / Wlan / PowrProf API 对接 Windows 原生能力（无 DLL 注入）。

---

## 1. 宿主启动、自愈与退出流程（`src/main.cpp`）

1. **CLI 与自愈入口**：支持 `--restore`（恢复原任务栏与工作区后立即退出）、`--watchdog <pid>`（轻量监听主进程意外退出并自动按 `recovery.dat` 还原桌面），以及单实例控制指令 `--show-overview`、`--settings`、`--reload`、`--quit`（通过隐藏控制窗类 `WINOME_ControlWindow` 投递消息）。
2. **单实例、配置与日志**：通过互斥体 `Local\WINOME_Shell_SingleInstance` 保证单实例；加载 `%LOCALAPPDATA%\WINOME\config.json`（`src/config.cpp`）与 i18n（`src/i18n.cpp`）；`setup_log_file()` 将 stdout / stderr 重定向至 `%LOCALAPPDATA%\WINOME\winome.log`（超过 5MB 自动轮转为 `winome.prev.log`）；注册 `SetUnhandledExceptionFilter` 生成 MiniDump 到 `crashes\` 并响应 `WM_QUERYENDSESSION` / `WM_ENDSESSION`。
3. **系统接入**：启用 Per-Monitor DPI Awareness v2（`src/host-utils.cpp`），尝试 `SeTcbPrivilege`，隐藏原生任务栏（`src/native-taskbar.cpp`），启动事件钩子（`src/win-event-hook.cpp`）与可配置的全局键盘钩子（`src/overview-trigger.cpp`）。
4. **多屏面板与概览**：创建主屏顶栏面板及副屏精简顶栏（`src/shell-panel.cpp`、`src/monitor-manager.cpp`），按各显示器独立保留工作区，初始化概览、全屏监视器（`src/fullscreen-watcher.cpp`）、左上角热角（`src/hot-corner.cpp`）、系统托盘采集（`src/systray.cpp`）与通知横幅服务（`src/notifications.cpp`）。
5. **延迟初始化与退出清理**：虚拟桌面 COM 延后至主循环启动后初始化并固定自身全部顶层窗；正常退出（`on_shutdown`）释放 `app_icon_cache_clear()`、撤销钩子并收拢工作线程、恢复工作区与原生任务栏并清理 `recovery.dat`。

---

## 2. 快捷键与热区触发

### 键盘钩子（`src/overview-trigger.cpp`）

- 吞掉单独的 `Win` `WM_KEYDOWN`，阻止系统直接弹出开始菜单；若在 `WM_KEYUP` 判定为裸 `Win` 按下松开：
  - 概览未打开时记录时间戳并打开概览（`WINDOW_PICKER`）；
  - 概览正在打开或处于 `WINDOW_PICKER` 且 **550ms 内再次按下裸 `Win`** 时，调用 `overview_can_shift_to_app_grid()` + `overview_shift_to_app_grid_async()` 丝滑打断过渡到 `APP_GRID`；其余情况关闭概览。
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

已全面替代早期基于 DLL 注入的 `extracted/WinOverview/` 方案，在宿主进程内基于 `overviewControls.js`（三态 `ControlsState::HIDDEN` / `WINDOW_PICKER` / `APP_GRID`）与 `workspace.js` 逐组件移植。

### 布局与交互模块

| 模块 | 上游参考 | 职责 |
|------|----------|------|
| `overview.cpp` | `overviewControls.js`, `windowPreview.js` | 搜索输入过滤（应用+打开窗口）、工作区背景盒与 0→30px 圆角插值（`APP_GRID` 时收起为顶部 15% 工作区缩略条）、单遍双槽位（`picker_rect` / `grid_rect`）计算、DWM 实时缩略图飞入/开合动画、`GtkGestureDrag` 跨桌面拖拽、中键关窗、chrome 淡出与 z-order 管理、快捷键（`Ctrl+F` / `Ctrl+Shift+N` / `PageUp` / `PageDown` / `Esc`） |
| `overview-layout.cpp` | `workspace.js` (`UnalignedLayoutStrategy`) | `lerp(1.5, 1)` 窗口缩放、行搜索最优 scale/space、`WINDOW_PREVIEW_MAXIMUM_SCALE = 0.95`、像素对齐、有效间距（主题 6px + chrome oversize） |
| `overview-dash.cpp` | `dash.js` | Favorites 固定列表持久化（`pinned_apps`）+ `.dash-separator` 分割线 + 未固定运行应用、右键菜单、9 点应用网格切换（Shift+点击回退开始菜单）、LRU 图标缓存与 300ms 悬停标签 |
| `app-index.cpp` | `appSystem.js`, `appSearch.js` | 异步索引开始菜单 `.lnk` 与 `FOLDERID_AppsFolder`（UWP/Store/系统应用、`PKEY_AppUserModel_ID`）、规范化去重与排序搜索、Win32/UWP/管理员启动与定位、`IShellItemImageFactory` → `SHIL_JUMBO` → `PrivateExtractIconsW` → `SHGetFileInfoW` 高清图标提取、`1024` 条上限 `GdkTexture` LRU 缓存与后台线程安全 `RawDecodedIcon` 解码 |
| `app-grid.cpp` | `appDisplay.js` | 宿主内分页应用网格（`.icon-grid` / `.overview-tile` / `WinomeAppIcon` + `GSK_SCALING_FILTER_TRILINEAR` / 按当前页优先的异步分批图标加载 / 结构缓存与 `app_grid_prewarm()` / `.app-grid-running-dot` 运行点指示 / 右键 AppMenu 上下文菜单与多窗口列表 / 滚轮与指示点翻页 / 响应式 6×4・5×3・4×3 排布） |
| `overview-thumbs.cpp` | `workspaceThumbnail.js` | 虚拟桌面壁纸药丸行（支持拖拽落点高亮、仅 1 个桌面拖拽时自动展开 `[Desktop 1] [+]`、`+` 新建与悬停 `×`/中键关闭桌面、活动桌面指示器与迷你 DWM 预览） |
| `ws-osd.cpp` | `workspaceSwitcherPopup.js` | 概览外 `Win+Ctrl+←/→` 切换桌面时，主屏底部中央（4em 边距）显示圆点 OSD（100ms EASE_OUT_QUAD 淡入 → 600ms 常驻 → 100ms 淡出，连续切换重置计时） |

### 关键实现细节

- **统一三态开合与打断过渡**：`ControlsState` 驱动 `HIDDEN ↔ WINDOW_PICKER ↔ APP_GRID` 在 `250ms` 帧时钟动画内无缝互转或中途打断（打开概览时提前调用 `app_grid_prewarm()` 预建/预解码当前页图标）；窗口以 `gtk_window_fullscreen` 创建保持 surface 与显示器一致；布局 idle 使用 `G_PRIORITY_HIGH_IDLE` 抢在首帧重绘前就绪，避免黑闪；关闭后保留控件几何以便重开首帧铺满壁纸。
- **最小化窗口**：使用 `GetWindowPlacement().rcNormalPosition` 作为原矩形并按恢复矩形归属显示器，避免 `GetWindowRect` 返回离屏坐标；开合动画从工作区角落展开并淡入；点击激活时先 `SW_RESTORE` 再 `SetForegroundWindow`。
- **排除自身窗口**：枚举窗口时用 `is_our_process()` 过滤本进程所有窗口（面板、弹出窗、概览与 chrome 窗口）。
- **悬停 chrome 独立顶层窗**：因为 DWM 缩略图合成在窗口自绘内容之上，关闭按钮（32px）与应用图标（64px，`WM_GETICON` / `IShellItemImageFactory`）置于独立透明顶层窗（`WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`）浮于缩略图之上，指针离开 `750ms` 后淡出；关闭按钮发送 `WM_CLOSE` 并触发重排。
- **工作区切换动画**（`switch_workspace_animated`）：概览内切换桌面时，旧桌面缩略图移入离场组，与新桌面同步横向滑动（`250ms EASE_OUT_CUBIC`），背景盒随行平移，缩略图指示器连续插值滑动；外部切换由 400ms 轮询跟随重排。

---

## 5. 虚拟桌面 COM 模块（`src/virtual-desktop.cpp`）

移植并扩展自 [VirtualDesktopAccessor-rust](https://github.com/Ciantic/VirtualDesktopAccessor)（MIT），在 Windows 11 22621 / 24H2+ 验证：

- **接口获取**：`CoIncrementMTAUsage` → `CLSID_ImmersiveShell` → `QueryService` 获取 `IVirtualDesktopManagerInternal`（枚举/切换/移动/增删）与 `IVirtualDesktopManager`（窗口归属）。
- **版本感知槽位表（`VtableProfile`）**：通过 `ntdll!RtlGetVersion` 读取 OS Build 号并由 `vtable_slots_for_build()` 动态绑定 Win11 `22621..25999` 与 `26100+`（24H2，slot 10 起偏移 `+1`）的方法槽位；`< 22621` 安全跳过创建/删除与内部视图调用。提供 `move_window_to_desktop()`、`create_desktop()`、`remove_desktop()` 与 `unpin_window()`，全部基于索引与 `IApplicationViewCollection` / `GetAt()` 执行。
- **调用顺序与稳定保护**：枚举优先于 `GetCurrentDesktop`；全部 COM 调用经裸函数指针（防 GCC LTO 去虚拟化）并在专用工作线程带 `8s` 超时顺序执行，异常或超时后平滑降级单工作区布局。
- **跨桌面固定（`pin_window`）**：通过 `IVirtualDesktopPinnedApps`（IID `4CE81583`）与 `IApplicationViewCollection`（IID `1841C6D7`）将壳层全部窗口固定到所有桌面。

---

## 6. Windows 原生数据源与系统集成

| 模块 | 实现机制 | 备注与降级策略 |
|------|----------|----------------|
| **Wi-Fi**（`src/wifi-networks.cpp`） | 运行时加载 `wlanapi.dll`，进程常驻 `WlanOpenHandle` 并注册 `WlanRegisterNotification` 事件通知 | 无网卡或缺失 `wlanapi.dll` 时安静降级；支持异步扫描、BSSID 信号合并、已存 Profile 与内嵌密码输入生成 WLAN XML 连接、断开网络 |
| **电源模式**（`src/power-modes.cpp`） | `PowrProf` + `powersetting.h` 枚举并切换节能 / 均衡（含 Win11 GUID）/ 性能 | `PowerEnumerate` 探测缺失的方案不显示；无法识别时副标题回退显示均衡 |
| **系统状态**（`src/system-status.cpp`） | 电量 `GetSystemPowerStatus`、音量 WASAPI `IAudioEndpointVolumeCallback`（事件驱动与静音切换）、屏幕亮度 WMI (`ROOT\WMI`) + `dxva2.dll` DDC/CI、电源操作自动申请 `SE_SHUTDOWN_NAME` | 外接显示器/台式机无物理亮度调节能力时动态隐藏亮度滑块行 |
| **WinRT 与通知中心**（`src/winrt-bridge.cpp`、`src/notifications.cpp`、`src/calendar.cpp`） | 异步探测并桥接 Windows Radio（蓝牙开关/已配对列表）、SMTC 媒体控制与 `UserNotificationListener` | 在顶栏弹出双栏 DateMenu（SMTC 媒体卡片 + 通知列表/勿扰开关 + 日历）并在非勿扰/非全屏下显示顶部通知横幅 |
| **系统托盘**（`src/systray.cpp`） | 代理提取 Windows 系统托盘常驻图标与文本提示并在面板状态区弹出展示 | 支持点击与右键消息转发，唤起第三方托盘程序窗口或菜单 |
| **多屏、配置与设置**（`src/monitor-manager.cpp`、`src/config.cpp`、`src/i18n.cpp`、`src/settings-window.cpp`） | `EnumDisplayMonitors` 管理多屏面板与 DPI 变化；`%LOCALAPPDATA%\WINOME\config.json` 热重载；深色 Adwaita GTK4 设置窗（深色标题栏与设置页卡片） | 支持单/全屏顶栏策略、热角与 Win 键策略、时钟/电量选项与桌面重启自恢复 |

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
