# 系统架构与模块设计

WINOME 在宿主单进程 `winome.exe` 内以 C++17 + GTK4 渲染完整 Shell UI，并直接通过 Win32 / COM / DWM / Wlan / PowrProf API 对接 Windows 原生能力（无 DLL 注入）。

---

## 1. 宿主启动、自愈与退出流程（`src/main.cpp`）

1. **CLI 与双向 IPC 入口**：支持 `--restore`（多遍 `force_restore_all()` + `ABM_GETSTATE` 恢复原生任务栏后立即退出）、`--watchdog <pid> [--no-crash-ui]`（基于 `RecoverySnapshot` / `CrashState` 自动还原任务栏并在异常退出时拉起零 GTK 依赖的纯 Win32 `show_crash_recovery_dialog` 操作窗口，内置 `diagnostics-export.{h,cpp}` 一键导出含版本/系统/多屏拓扑/日志/转储的 `.zip` 排障压缩包）、`--safe-mode`（跳过外置插件与窗口外观注入）、`--after-pid <pid>`（供 `Alt+F2 -> r` 无竞态平滑重启）、`--status`（经 Named Pipe 查询运行中宿主遥测与健康状态 JSON），以及 `--crash-ui`、`--show-overview`、`--settings`、`--reload`、`--quit`；`src/shell-ipc.{h,cpp}` 通过 `ipc_build_user_pipe_sddl()`（当前用户 SID + `S:(ML;;NW;;;ME)`）保护 Named Pipe `\\.\pipe\WINOME_Shell_IPC`，供无 UAC 清单的 `WINOME/ctl/winome-ctl.exe` 在普通用户权限下发送 `ping`/`status`/`logs.*`/`debug.flags.*`/`subsystem.*`/`inspect.*`/`overview.*`/`desktop.*`/`theme.reload`/`plugin.*`/`looking_glass.*`/`run.exec`/`quit` 等 JSON 指令。
2. **单实例、配置、遥测、实时日志与崩溃钩子**：通过互斥体 `Local\WINOME_Shell_SingleInstance` 保证单实例；加载 `%LOCALAPPDATA%\WINOME\config.json`（含 `developer_mode` 与 `disabled_plugins`）；`src/perf-metrics.{h,cpp}` 维护启动遥测与子系统熔断器；`src/debug-runtime.{h,cpp}` 通过 `debug_install_log_capture()` 将 `g_print`/`g_printerr` 与插件日志写入 512 条内存环形缓冲区（并发追加写入 `%LOCALAPPDATA%\WINOME\winome.log`，`5MB` 自动轮转 `winome.prev.log`）；注册 `SetUnhandledExceptionFilter`、`std::set_terminate` 与 `signal(SIGABRT/SIGSEGV/SIGILL/SIGFPE)` 写入 `MiniDump`、`crash-state.json` 并执行无锁 `restore_emergency_no_join()`，同时保留 `recovery.dat` 交给 `--watchdog` 完成多遍自愈兜底。
3. **系统接入**：启用 Per-Monitor DPI Awareness v2（`src/host-utils.cpp`），尝试 `SeTcbPrivilege`，隐藏原生任务栏并快照初始 `ABM_GETSTATE`（`src/native-taskbar.cpp`），启动事件钩子（`src/win-event-hook.cpp`）、可选零注入 DWM 窗口外观统一（`src/window-theming.{h,cpp}`）与全局键盘钩子（`src/overview-trigger.cpp`，涵盖 `Alt+F2` 运行弹窗与虚拟桌面热键）。
4. **多屏面板、运行弹窗、插件宿主机与概览**：创建主/副屏顶栏面板并在主屏 `right_box` 预留 `#panelPlugins` 声明式扩展区；接入 60s 会话确认弹窗（`end-session-dialog`）、`Alt+F2` 运行弹窗（`run-dialog`）、Looking Glass 开发者控制台（`looking-glass`）、`plugin-host` 进程外 stdio JSON 插件宿主机（非安全模式下扫描 `%LOCALAPPDATA%\WINOME\plugins\*\manifest.json` 并在 Win32 Job Object 内隔离运行，通过 `PluginRateLimiter(30, 1000)` 与 `PluginCrashTracker(3, 60000)` 约束行为并在 TopBar/QuickSettings 动态生成 UI 控件）。
5. **延迟初始化与退出清理**：虚拟桌面 COM 延后至主循环启动后初始化并固定自身全部顶层窗；正常退出（`on_shutdown`）依次关闭插件宿主机（优雅发送 `shutdown` JSON 后由 Job Object 收尾）、停止 `ipc_server_stop()`、关闭控制窗与各大对话框/控制台、释放 `window_theming_shutdown()` 与 `app_icon_cache_clear()`、撤销钩子、恢复 `ABM_GETSTATE` 与原生任务栏并清理 `recovery.dat`。

---

## 2. 快捷键与热区触发

### 键盘钩子（`src/overview-trigger.cpp`）

- 吞掉单独的 `Win` `WM_KEYDOWN`，阻止系统直接弹出开始菜单；若在 `WM_KEYUP` 判定为裸 `Win` 按下松开：
  - 概览未打开时记录时间戳并打开概览（`WINDOW_PICKER`）；
  - 概览正在打开或处于 `WINDOW_PICKER` 且 **550ms 内再次按下裸 `Win`** 时，调用 `overview_can_shift_to_app_grid()` + `overview_shift_to_app_grid_async()` 丝滑打断过渡到 `APP_GRID`；其余情况关闭概览。
- `Win+PageUp` / `Win+PageDown` 切换到相邻虚拟桌面；`Win+Shift+PageUp` / `Win+Shift+PageDown` 将当前前景应用窗口移动到相邻桌面并跟随切换（均投递给持久化 `HotkeyWorker` 后台线程异步执行，绝不阻塞 `WH_KEYBOARD_LL`）。
- 仅无修饰键的 `Win+Tab` 通过 `SendInput` 原子合成带标记的 `[LWIN DOWN, LWIN UP]` 打开开始菜单（屏蔽长按重复并吞掉配对 `Tab` 抬起；若概览已开则先安全收起再打开开始菜单）。
- 其余与 WINOME 不冲突的 `Win+<key>` 组合键（如 `Win+V` 剪贴板、`Win+E`、`Win+R`、`Win+D`、`Win+Shift+S`、`Win+Ctrl+←/→/D/F4` 并同步发送 `vd::notify_change_hint_async()`、`Win+Alt+Tab`、`Win+Shift+Tab` 等）：对先按下的 `Ctrl/Shift/Alt` 修饰键直接透传，待非修饰触发键按下时通过单次 `SendInput(2, ...)` 原子注入 `[g_win_vk DOWN, 触发键 DOWN]` 并吞掉乱序原始触发键按下，确保 `Win` 严格先于目标键到达系统；且未回注前绝不误用 `GetAsyncKeyState(g_win_vk)` 重置状态。
- `Esc` 在概览激活时关闭概览。

### 左上角热角（`src/hot-corner.cpp`）

独立线程监听 `WH_MOUSE_LL` 指针坐标，通过 SeqLock 维护最多 16 块显示器的热角矩形（纯函数 `hot_corner_hit_test` 校验），指针进入任意未全屏显示器的左上角 `5px` 热角即可在对应显示器独立触发概览（默认 `2s` 冷却）；当开关关闭或所有面板全屏隐藏时自动摘除鼠标钩子，空闲与全屏游戏零开销。

---

## 3. 顶部面板与弹出窗（`src/shell-panel.cpp`）

- **布局移植**：自定义 `WinomePanelLayout` 复刻 `panel.js` `vfunc_allocate()`，保证中间时钟相对显示器工作区严格居中，左右侧盒钳制在各自自然宽度（避免 `GtkCenterBox` 在剩余空间居中导致时钟偏移）。
- **图层与置顶**：面板与弹出窗各自作为独立无边框顶层窗口（`shadow-wrap` 卡片），由 `WH_MOUSE_LL` 钩子处理外部点击关闭；`WS_EX_TOOLWINDOW` 由 `fullscreen-watcher` 周期性重断言防止被 GDK configure 重置。
- **层级契约**：`popover > panel > 概览 chrome 窗口 > 概览窗口 > 应用 > 壁纸`。
- **与概览协作**：
  - 打开概览时关闭已开弹出窗，顶栏背景按概览过渡进度帧同步插值（`#000000 ↔ #222226`），左侧 `WinomeActivitiesBtn` 指示点随概览内连续工作区切换进度实时平滑形变（对齐上游 `WorkspaceDot`）；
  - 允许在概览上方直接打开快速设置或日历；
  - 弹出窗被外部点击关闭时，通过 `winome_shell_panel_popover_just_closed()` 通知概览吞掉该次点击（对应 GNOME 菜单 grab 语义），不连带关闭概览。

---

## 4. 原生 Activities 概览（`src/overview*.cpp`）

已全面替代早期基于 DLL 注入的 `extracted/WinOverview/` 方案，在宿主进程内基于 `overviewControls.js`（三态 `ControlsState::HIDDEN` / `WINDOW_PICKER` / `APP_GRID`）与 `workspace.js` 逐组件移植。

### 布局与交互模块

| 模块 | 上游参考 | 职责 |
|------|----------|------|
| `overview.cpp` | `overviewControls.js`, `windowPreview.js` | 搜索输入过滤（应用+打开窗口）、工作区背景盒与 0→30px 圆角插值（`APP_GRID` 时收起为顶部 15% 工作区缩略条）、单遍双槽位（`picker_rect` / `grid_rect`）计算、DWM 实时缩略图飞入/开合动画、`GtkGestureDrag` 同屏/跨屏拖拽、桌面实时窗口拖入（经 `EVENT_SYSTEM_MOVESIZESTART/END`）、中键关窗、chrome 淡出与 z-order 管理、快捷键（`Ctrl+F` / `Ctrl+Shift+N` / `PageUp` / `PageDown` / `Esc`） |
| `overview-secondary.cpp` | 多显示器同时概览 | 独立或联动展开任意副屏概览（默认 `multi_monitor_overview_linked=false` 支持各屏同时独立开合，也可切换全屏联动），支持顶部工作区胶囊、侧边露出相邻工作区、副屏窗口预览点击/中键关闭/跨屏拖拽、实时蓝色落点高亮与显示器热插拔自动清理重定位 |
| `overview-layout.cpp` | `workspace.js` (`UnalignedLayoutStrategy`) | `lerp(1.5, 1)` 窗口缩放、行搜索最优 scale/space、`WINDOW_PREVIEW_MAXIMUM_SCALE = 0.95`、像素对齐、有效间距（主题 6px + chrome oversize） |
| `overview-dash.cpp` | `dash.js` | Favorites 固定列表持久化（`pinned_apps`）+ `.dash-separator` 分割线 + 未固定运行应用、右键菜单、9 点应用网格切换（Shift+点击回退开始菜单）、LRU 图标缓存与 300ms 悬停标签 |
| `app-index.cpp` | `appSystem.js`, `appSearch.js` | 异步索引开始菜单 `.lnk`/`.url`（保留 User/Common `Programs` 目录顺序、`folder_name`/`folder_order`/`item_order`）、原生 AES-256-CBC/MT19937 解密 Win11 `start2.bin`（`decrypt_start2_bin_bytes()`，完整还原固定项与 `pinType: 2` 文件夹编组，含 `KnownFolder` `{GUID}` 与 `Export-StartLayout` 回退）及 `FOLDERID_AppsFolder`（附加 Win32 AUMID、`PKEY_AppUserModel_ID`）、`build_app_grid_items()` 两级单向同步编排（开始菜单固定配置优先 → 未固定项按目录顺序自动分组，严格单向不回写磁盘）、`FindFirstChangeNotificationW` 目录与 `start2.bin` 单向监听、`1024` 条上限 `GdkTexture` LRU 缓存与后台 `RawDecodedIcon` 解码 |
| `app-grid.cpp` | `appDisplay.js` | 宿主内分页应用网格（`.icon-grid` / `.overview-tile` / `.app-folder` 2×2 缩略预览 / 居中 `.app-folder-dialog` 文件夹弹层与 `.folder-name-container` / `.folder-name-label`、`Esc` 或点击外部关闭 / `WinomeAppIcon` + `GSK_SCALING_FILTER_TRILINEAR` / 按当前页优先的异步分批图标加载 / 结构缓存与 `app_grid_prewarm()` / `.app-grid-running-dot` 运行点指示 / 右键 AppMenu 上下文菜单 / 滚轮与指示点翻页 / 响应式 6×4・5×3・4×3 排布） |
| `overview-thumbs.cpp` | `workspaceThumbnail.js` | 虚拟桌面壁纸药丸行（支持拖拽落点高亮、仅 1 个桌面拖拽时自动展开 `[Desktop 1] [+]`、`+` 新建与悬停 `×`/中键关闭桌面、活动桌面指示器与迷你 DWM 预览） |
| `live-wallpaper.cpp` | 第三方动态壁纸引擎兼容（Option C 混合方案） | 快速探测 `Progman`/`WorkerW` 下的 Wallpaper Engine / Lively / Sucrose 渲染窗口，通过无边框 `Windows.Graphics.Capture`（`IsBorderRequired=false`）在概览内 30 FPS 实时同步或抓帧（受 `live_wallpaper_mode` 控制，静态壁纸场景零开销） |
| `ws-osd.cpp` | `workspaceSwitcherPopup.js` | 概览外切换桌面时，通过 `vd::add_change_listener()` 在主屏底部中央显示透明胶囊圆点 OSD（`#winomeWsOsdWindow` 透明顶层窗 + 禁用 Win11 DWM 非客户区边框/圆角 + `WS_EX_TRANSPARENT` 鼠标穿透；100ms EASE_OUT_QUAD 淡入 → 600ms 常驻 → 100ms 淡出，连续切换重置计时） |

### 关键实现细节

- **统一三态开合与打断过渡**：`ControlsState` 驱动 `HIDDEN ↔ WINDOW_PICKER ↔ APP_GRID` 在 `250ms` 帧时钟动画内无缝互转或中途打断（打开概览时提前调用 `app_grid_prewarm()` 预建/预解码当前页图标）；窗口以 `gtk_window_fullscreen` 创建保持 surface 与显示器一致；布局 idle 使用 `G_PRIORITY_HIGH_IDLE` 抢在首帧重绘前就绪，避免黑闪；关闭后保留控件几何并在闲置 `15s` 后自动执行 `app_icon_cache_trim(256)` + `shrink_to_fit()` 释放非热点纹理；壁纸解码按目标显示器缩放加载并改用非弃用的 `gdk_memory_texture_new`。
- **最小化窗口**：使用 `GetWindowPlacement().rcNormalPosition` 作为原矩形并按恢复矩形归属显示器，避免 `GetWindowRect` 返回离屏坐标；开合动画从工作区角落展开并淡入；点击激活时先 `SW_RESTORE` 再 `SetForegroundWindow`。
- **排除自身窗口与默认黑名单**：枚举窗口时用 `is_our_process()` 过滤本进程所有窗口（面板、弹出窗、概览与 chrome 窗口），并在 `config.json` 开启 `overview_window_blacklist`（默认 `true`，可在 `Settings -> Behavior` 关闭）时通过 `src/host-utils.{h,cpp}` 提供的 `is_overview_default_blacklisted_hwnd()` / `is_overview_default_blacklisted_window_info()` 在 `overview.cpp`（主概览与迷你预览）、`overview-dash.cpp` 与 `app-index.cpp` 统一过滤“Windows 输入体验”（`TextInputHost.exe` / `Windows.UI.Core.CoreWindow`）及 `explorer.exe` 下空白无名幽灵窗（被 cloak 或非 `CabinetWClass`/`ExploreWClass`/`#32770`）。
- **悬停 chrome 独立顶层窗**：因为 DWM 缩略图合成在窗口自绘内容之上，关闭按钮（32px）与应用图标（64px，`WM_GETICON` / `IShellItemImageFactory`）置于独立透明顶层窗（`WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`）浮于缩略图之上，指针离开 `750ms` 后淡出；关闭按钮发送 `WM_CLOSE` 并触发重排。
- **工作区切换动画与事件同步**（`switch_workspace_animated`）：概览内切换桌面时，旧桌面缩略图移入离场组，与新桌面同步横向滑动（`250ms EASE_OUT_CUBIC`），背景盒随行平移，缩略图指示器连续插值滑动；概览、`shell-panel` 与 `ws-osd` 统一订阅 `vd::add_change_listener()`，不再各自维持定时器轮询。

---

## 5. 多屏工作区引擎与虚拟桌面 COM（`src/workspace-manager.cpp`、`src/virtual-desktop.cpp`）

在底层虚拟桌面 COM 之上新增 `src/workspace-manager.{h,cpp}`，提供三档可热切换的多显示器工作区模式（`config.json` `multi_monitor_workspace_mode`）：
- **`independent`（默认，WINOME 内置每屏独立工作区引擎）**：将 Windows 原生桌面 0 作为全局常驻 Live Stage，原生桌面 1 作为停泊桌面（Parking Desktop），通过 `vd::move_windows_batch()` 单遍批量隐藏/还原目标显示器的窗口，使各块显示器拥有完全独立的工作区列表与活动索引（屏幕 1 切换工作区绝不影响屏幕 2），并支持外部点击/通知激活停泊窗口自动跟随、跨屏窗口迁移（`ws::transfer_window_to_monitor_workspace`）以及退出/拔屏时全量还原所有停泊窗口；
- **`primary_only`（GNOME 默认）**：工作区仅作用于主显示器，自动跨桌面固定副屏窗口并在切回或退出时解除固定；
- **`span`（全局同步）**：所有显示器跟随 Windows 原生虚拟桌面同步切换。

底层虚拟桌面模块移植并扩展自 [VirtualDesktopAccessor-rust](https://github.com/Ciantic/VirtualDesktopAccessor)（MIT），在 Windows 11 22621 / 24H2+ 验证：

- **接口获取**：`CoIncrementMTAUsage` → `CLSID_ImmersiveShell` → `QueryService` 获取 `IVirtualDesktopManagerInternal`（枚举/切换/移动/增删）与 `IVirtualDesktopManager`（窗口归属）。
- **版本感知槽位表（`VtableProfile`）**：通过 `ntdll!RtlGetVersion` 读取 OS Build 号并由 `vtable_slots_for_build()` 动态绑定 Win11 `22621..25999` 与 `26100+`（24H2，slot 10 起偏移 `+1`）的方法槽位；`< 22621` 安全跳过创建/删除与内部视图调用。提供 `move_window_to_desktop()`、`create_desktop()`、`remove_desktop()` 与 `unpin_window()`，全部基于索引与 `IApplicationViewCollection` / `GetAt()` 执行。
- **调用顺序、稳定保护与集中式观察者**：枚举优先于 `GetCurrentDesktop`；全部 COM 调用经裸函数指针（防 GCC LTO 去虚拟化）并在专用工作线程（`std::shared_ptr<Job>` 防超时竞态销毁）带 `8s` 超时顺序执行，异常或超时触发 `SubsystemId::VirtualDesktop` 熔断器并平滑降级单工作区布局；内置单一共享 `EVENT_SYSTEM_DESKTOPSWITCH` + 热键提示 + `1.5s` 安全校对器广播变更给订阅者。
- **跨桌面固定（`pin_window`）**：通过 `IVirtualDesktopPinnedApps`（IID `4CE81583`）与 `IApplicationViewCollection`（IID `1841C6D7`）将壳层全部窗口固定到所有桌面。

---

## 6. Windows 原生数据源与系统集成

| 模块 | 实现机制 | 备注与降级策略 |
|------|----------|----------------|
| **Wi-Fi**（`src/wifi-networks.cpp`） | 运行时加载 `wlanapi.dll`，进程常驻 `WlanOpenHandle` 并注册 `WlanRegisterNotification` 事件通知；快速设置抽屉打开期间由事件监听 + 4s 后台扫描异步实时刷新 | 无网卡或缺失 `wlanapi.dll` 时触发状态报告并安静降级；支持异步扫描、BSSID 信号合并、已存 Profile 与内嵌密码输入生成 WLAN XML 连接、断开网络 |
| **电源模式**（`src/power-modes.cpp`） | 动态加载 `powrprof.dll` 的 Windows 10/11 Power Overlay Schemes（`PowerGetEffectiveOverlayScheme` / `PowerGetActualOverlayScheme` / `PowerSetActiveOverlayScheme`）实现节能 / 均衡 / 性能三档与系统设置双向同步 | 若当前为旧版非平衡基础计划则自动切回平衡计划使覆盖方案生效；不支持 Overlay 的旧系统自动回退传统 `PowerEnumerate`/`PowerSetActiveScheme` |
| **系统状态、明暗主题与会话弹窗**（`src/system-status.cpp`、`src/end-session-dialog.cpp`） | 电量 `GetSystemPowerStatus`、音量 WASAPI `IAudioEndpointVolumeCallback`（事件驱动与静音切换）、屏幕亮度 WMI (`ROOT\WMI`) + `dxva2.dll` DDC/CI、细粒度独立 `AppsUseLightTheme` / `SystemUsesLightTheme` 注册表读写（绝不创建或覆盖 Windows `.theme` 与幻灯片壁纸）、蓝牙抽屉 3s/9s 异步刷新、电源操作自动申请 `SE_SHUTDOWN_NAME` | 外接显示器/台式机无物理亮度调节能力时动态隐藏亮度滑块行；Restart / Power Off / Log Out 在 `end_session_confirm` 开启时先显示 GNOME `.modal-dialog` 60 秒确认弹窗 |
| **WinRT 与通知中心**（`src/winrt-bridge.cpp`、`src/notifications.cpp`、`src/calendar.cpp`） | 异步桥接 Windows Radio（蓝牙开关/已配对列表）、SMTC 媒体控制与 `UserNotificationListener`（`std::shared_ptr<State>` 防超时 UAF、`winrt_acquire_com_getter_safe()` 补偿 Win11 `wpnapps.dll!NotificationVisual::get_Bindings` 漏 `AddRef`、只读 `wpndatabase.db` SQLite 回退与 XML 元数据补全、Win11 逐 ID `RemoveNotification` 联动清除），并支持点击通知卡片/横幅/操作按钮通过协议 URI、`IApplicationActivationManager` 深链、Win32 窗口唤醒或 `AppIndex` 打开对应应用 | 在顶栏弹出双栏 DateMenu（SMTC 媒体卡片 + 可点击通知列表/操作按钮/勿扰开关 + 日历）并在非勿扰/非全屏下显示顶部通知横幅 |
| **系统托盘**（`src/systray.cpp`、`src/native-taskbar.cpp`） | 混合采集经典 `ToolbarWindow32` 与 Win11 XAML（`DesktopWindowContentBridge` 中的 `NotifyItemIcon` / `TopLevelWindowForOverflowXamlIsland` / `NotifyIconOverflowWindow`）第三方托盘图标；拆分为独立 `action_worker`（高优点击/溢出切换）与 `scan_worker`（`5s` 巡检并保留 `last_xo_items`，`shell_tray_mu` 保护显隐切换），配置 `IUIAutomation2`（`800ms`/`1500ms` 超时）与 `FindAllBuildCache` 批量拉取属性，并以 `entry.id`/标题双重内存缓存来自 `HKCU\Control Panel\NotifyIconSettings` 的真实图标 | 按 `TraySurface` 直达激活（溢出项免盲扫 `Shell_TrayWnd`），点击/右键立即收起 GTK 浮层并 `AllowSetForegroundWindow(ASFW_ANY)`，优先 `IUIAutomationElement3::ShowContextMenu()`（失败回退 `Shift+F10`/物理右键，右键保活 `1500ms` + `#32768` 检测）；`inspect.tray` 为非阻塞瞬时快照（`SendMessageTimeoutW` 50ms + 缓存 `uia_probe`），`winome-ctl` 启用 `PeekNamedPipe` 回复超时保护 |
| **多屏、外观统一、配置与设置**（`src/monitor-manager.cpp`、`src/window-theming.cpp`、`src/config.cpp`、`src/i18n.cpp`、`src/settings-window.cpp`） | `EnumDisplayMonitors` 与 `SPI_SETWORKAREA`（`on_workarea_changed`）管理多屏面板与 DPI 变化；`window-theming` 通过 `SetWinEventHook` 零注入应用 DWM 暗色标题栏/圆角（默认关闭）；`%LOCALAPPDATA%\WINOME\config.json` 热重载；深色 Adwaita GTK4 设置窗 | 支持单/全屏顶栏策略、热角与 Win 键策略、概览默认黑名单（`overview_window_blacklist`）、可选窗口外观统一、时钟/电量选项、`Diagnostics` 遥测页与桌面重启自恢复 |

---

## 7. 性能与权限基线

### 性能优化措施

- **动画走帧时钟与帧预算监控**：概览开合、悬停 chrome、菜单开合均使用 `gtk_widget_add_tick_callback`（对齐显示器 vblank），`overview.cpp` 以 `FrameTimeTracker(20000)` 统计平均/P95/最大帧耗时与掉帧计数。
- **z-order 漂移门控与事件化自愈**：`fullscreen-watcher` 接入 `EVENT_SYSTEM_MINIMIZEEND` 与工作区变更即时重断言，并将后台维护巡检 `maintenance_tick` 放宽至 `10s`；重断言前校验 `popover > panel > chrome > overview` 链，链路完整即跳过 `SetWindowPos`。
- **隔离 Fontconfig 与空闲纹理收缩**：`fonts.cpp` 使用 `FcConfigCreate()` 隔离 Fontconfig 实例并加载捆绑字体与精选 CJK/Emoji 回退链（测试进程 `winome-tests` 测得 `57.8ms`–`58.3ms`，主宿主场景实测 `1226.1ms`）；概览关闭闲置 `15s` 后收敛图标缓存至 256 条并收缩桌面壁纸按屏解码，纯透明淡入跳过 `gtk_fixed_move`。
- **GPU 渲染与产物基线**：
  - 关键窗口由 OpenGL（`GskGLRenderer` / `ngl`）硬件绘制，`[gpu]` 启动日志记录渲染器与 GL 厂商；
  - **测试构建（`test` / `debugoptimized`）**：`winome.exe` 约 `22MB`；
  - **生产构建（`release` + LTO + NDEBUG）**：`winome.exe` 约 `2.7–2.9MB`（全量便携包 `WINOME-win64-portable.zip` 约 `25–26MB`，含 `winome-ctl.exe` 与示例插件）。

### 权限要求

- 隐藏原生任务栏、操作 Explorer 窗口及完整使用虚拟桌面 COM 服务需要以**管理员权限**运行 `winome.exe`。
- 未提权运行时，若 Explorer RPC 死锁，VD 模块将在 8s 超时后自动禁用并回退单工作区，保证 UI 不崩溃、不冻结。
