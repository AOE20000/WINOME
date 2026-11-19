# 开发备忘、硬约束与已踩坑点

本文汇总开发过程中沉淀的不可退让硬约束、Win32/GTK4/CSS 踩坑记录及明确暂缓的功能评估结论。

---

## 1. 工程硬约束（已踩深坑，勿回退）

1. **VD COM 必须走裸 vtable 函数指针**：GCC `-O2` 会对纯虚接口做去虚拟化，将调用替换为 `__cxa_pure_virtual`，MinGW 链接器将该弱符号解析到非法地址导致运行时 `SIGSEGV`。
2. **VD COM 只在一个专用工作线程调用**：主线程等待完成并设置 `8s` 超时，超时后永久禁用 VD 模块并降级单工作区，绝不冻结 UI。
3. **VD 枚举必须先于查当前桌面**：Windows 11 22621 上直接先调 `GetCurrentDesktop` 会使 explorer 服务挂起。
4. **VD 槽位布局兼容 22621 与 24H2**：两者接口 IID 相同（`53F5CA0B`），24H2 在 slot 10 插入了新方法导致其后槽位全部 `+1`；仅使用偏移区前的稳定槽位（`GetDesktops` / `GetCurrentDesktop` / `SwitchDesktop` + `GetAt`），绝不直接调用 `find_desktop`。
5. **动画必须走帧时钟**：使用 `gtk_widget_add_tick_callback` 对齐 vblank，不用固定 16ms `g_timeout_add`。
6. **z-order 重排必须有漂移门控**：重断言前先用 `zorder_drifted()` 检查 `popover > panel > chrome > overview` 链，链路完整即跳过 `SetWindowPos`，避免无谓的 `WM_NCCALCSIZE` 往返。
7. **无 GTK 主题环境下控件自绘**：`GtkScale` 等依赖系统 GTK 主题的控件在本环境会退化失效，使用自绘（如 `WinomeQuickSlider`）而非尝试更换 GTK 主题。
8. **按钮必须关闭点击聚焦**：`gtk_widget_set_focus_on_click(FALSE)`，避免 `:focus` 与 `:hover` 共用高亮导致点击后高亮粘滞。
9. **高频路径重绘守卫**：图标刷新前检查名称是否变化（`set_icon_name_if_changed`）；纯透明度渐变帧跳过 `gtk_fixed_move`，避免触发无意义容器 relayout。
10. **启动关键路径避开 COM**：VD 首次枚举延后 1.5s–2s、壳层窗口 pin 延后 3s，防止低完整性下 explorer RPC 死锁卡住启动。
11. **异步超时任务禁止捕获栈引用**：带超时的后台任务（如 `winrt-bridge.cpp`、8s 超时的 `virtual-desktop.cpp`）若主线程先返回，栈引用立即悬空——状态/任务体必须使用 `std::shared_ptr` 托管。
12. **键盘钩子禁止同步调用 Explorer COM**：`WH_KEYBOARD_LL` 超时会触发 Windows 自动摘钩；`Win(+Shift)+PageUp/PageDown` 等跨桌面快捷键必须投递到持久化 `HotkeyWorker` 线程异步执行。
13. **避免已弃用 GTK4 纹理 API**：GTK 4.10+ 在 `warning_level=2` 下会对 `gdk_texture_new_for_pixbuf` 触发 `-Wdeprecated-declarations`，解码像素应直接使用 `gdk_pixbuf_get_pixels` + `g_bytes_new` + `gdk_memory_texture_new`。

---

## 2. Win32 API 踩坑

- **按需系统 DLL 必须动态加载**：`wlanapi.dll` 在无无线网卡的机器上不存在，静态链接会导致进程无法启动；用 `LoadLibraryW` + `GetProcAddress` 解析入口并做静默降级。
- **`GetProcAddress` 转换消除 `-Wcast-function-type`**：将 `FARPROC` 先转为 `void*` 再 `reinterpret_cast` 为目标函数指针，消除编译器警告。
- **动态解析签名无编译期校验**：`WlanOpenHandle` 只有 **4 个参数** `(DWORD, PVOID, PDWORD, PHANDLE)`；`WlanGetAvailableNetworkList` 第三参是保留位必须传 `0`（无 `*_ALL` 常量），出参为 `PWLAN_AVAILABLE_NETWORK_LIST`。签名或参数顺位写错不会在链接期报错，只会在运行时返回异常错误码。
- **802.11 SSID 原始字节解码**：`ucSSID` 不是 NUL 结尾文本，`uSSIDLength` 是字节数；中文 SSID 常为 UTF-8（4 个汉字占 12 字节），也有 Latin-1。应先校验 UTF-8，失败再按 Latin-1 展开，并预留 `97` 字节缓冲区（32 字节最坏展开 96 字节 + NUL）。
- **WLAN 同名 BSSID 去重与扫描缓存**：Windows 按 BSSID 返回多行，需按 SSID 合并保留最强信号；可见网络列表受 Windows 驱动扫描缓存影响，有时仅返回当前 AP 的若干 BSSID，每次打开菜单重新扫描即可。
- **PowrProf 方案 GUID 与头文件**：节能 `a1841308…`、均衡 `381b4222…`（Win11 另有随机化均衡 GUID `b9a9a03a…`）、性能 `8c5e7fda…`；`PowerEnumerate` 首参 `HKEY` 传 `NULL`、枚举标志用 `ACCESS_SCHEME`；`PowerGetActiveScheme(HKEY, GUID**)` 位于 `powersetting.h`，返回的 GUID 需 `LocalFree`。
- **检查 Win32 `BOOL` 返回值**：`ExitWindowsEx`、`SetSuspendState` 等操作受会话权限与电源策略限制，务必检查返回状态。
- **UAC 清单与非提权 Named Pipe 访问**：`winome.exe` 清单声明 `requireAdministrator`，非提权终端直接运行 `winome.exe --status` 会被 UAC 阻拦；应使用不带 UAC 清单的 `winome-ctl.exe`，并配合服务端 `ipc_build_user_pipe_sddl()` 设置的 `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<USER_SID>)S:(ML;;NW;;;ME)` 让同用户 Medium-IL 进程免提权稳定连通 `\\.\pipe\WINOME_Shell_IPC`。
- **MinGW `<windows.h>` 低级键盘钩子常量**：MinGW 头文件未定义 `LLKHF_REPEAT`，判断键盘钩子自动连发必须像 `overview-trigger.cpp` 那样自行维护按键状态位（如 `g_alt_f2_down`）。
- **子进程线程 `wait_worker` 自销毁规则**：`plugin-host.cpp` 中若子进程自然崩溃退出而无外部停止方执行 `wait_thread.join()`，`wait_worker` 退出销毁最后一份 `shared_ptr<PluginRun>` 前必须在互斥锁下自行 `detach()` 未被接管的 `std::thread`，否则 C++ 会对仍处于 `joinable()` 的线程触发 `std::terminate()`。
- **优雅平滑重启单实例锁竞态**：触发 `Alt+F2 -> r` 重启时，新进程必须先经 `--after-pid <old_pid>` 等待旧实例释放 `Local\WINOME_Shell_SingleInstance` 后再抢锁，否则会被判定为重复实例而直接唤醒旧进程后自退。
- **Win32 拥有窗口（Owner-Owned）Z-Order 禁忌**：`g_chrome_hwnd` / `g_icon_hwnd` / `g_caption_hwnd` 以 `g_overview_hwnd` 为 Owner，不可先放子窗口再 `SetWindowPos(owner, child, ...)`，否则 Win32 强制重排并将面板压到概览下面（`contract=BROKEN`）；正确顺序为先置 `g_overview_hwnd` 于 `g_panel_hwnd` 之后，再用 `SWP_NOOWNERZORDER` 将各悬停 Chrome 窗夹在 `panel` 与 `overview` 之间。
- **VD 工作线程并发入队与退场 join**：`virtual-desktop.cpp` 原单槽位 `g_pending` 会在 `obs_worker` / `hotkey_worker` / UI 并发时覆盖丢单导致误触发 `8s` 假超时与永久 `Tripped`，须使用 FIFO `std::deque<Job>`；退出时 `ws_osd_shutdown()` 与 `ObserverState::~ObserverState()` 必须收拢 `worker` 线程，避免静态析构触发 `std::terminate()`。
- **UI 线程禁止同步调用 `ShellExecuteW` / `PangoWin32` 回退**：`app_index_launch` 与 `run-dialog` 外部启动必须在带 STA `CoInitializeEx` 的后台线程执行，否则 DDE/Shell 挂起会卡死 GTK 主循环并导致 `overview-trigger` 时序错乱；`fonts.cpp` 调用 `pango_cairo_font_map_set_default` 确保 GTK 内置 Tooltip / `GtkPopover` 不再回退到 `PangoWin32`（`pango_win32_font_logfont` 断言）；`GtkDrawingArea` 不可设置 `-1` 负 margin（会触发 `min 5 < 6` 布局警告）。
- **Win11 UIA 系统托盘防 20s–412s 级联超时**：`Shell_TrayWnd` 根窗 `FindAll(TreeScope_Descendants, TrueCondition)` 会遍历全部任务栏应用与多级子桥接且 Windows UIA 默认 `TransactionTimeout` 为 `20s`，若 IPC 线程同步 probe 并与后台轮询并发操作 `Shell_TrayWnd` 显隐/矩形会触发级联 COM 卡顿（实测高达 `412s`）并拖慢右键 10s。必须配置 `IUIAutomation2` 连接/事务超时（`800ms`/`1500ms`）、仅对 `DesktopWindowContentBridge` 子窗使用 `FindAllBuildCache` 批量取值、将用户动作与周期扫描拆分为双 MTA 线程（`shell_tray_mu` 串行保护），且 `systray_debug_inspect_json()` 仅非阻塞返回缓存状态与 `SendMessageTimeoutW` 子窗结果、`winome-ctl` 通过 `PeekNamedPipe` 限定回复超时。
- **Win11 概览幽灵系统窗口识别**：`TextInputHost.exe`（`Windows.UI.Core.CoreWindow` / `"Windows 输入体验"`）与 `explorer.exe` 无标题 cloaked `ApplicationFrameWindow` / `WorkerW` 均带 `WS_VISIBLE`、无 `WS_EX_TOOLWINDOW` 且由 VirtualDesktop API 判定在当前桌面可见，需通过 `host-utils` 默认黑名单（`overview_window_blacklist`）在概览、Dash 与应用运行点枚举阶段统一排除，避免误删真实 `CabinetWClass` 资源管理器与 `ApplicationFrameHost.exe` 托管 UWP 应用。

---

## 3. GTK4 浮层与事件机制踩坑

- **未挂入顶层窗口的控件永不显示**：`gtk_widget_set_visible(w, TRUE)` 对孤立 widget 无效；所有弹出层统一走 `shell-panel.cpp` 的自绘无边框顶层窗口（不用 `GtkPopover`，避免 Windows 下 grab 窗口导致 `:hover` 粘住）。
- **嵌套点击必须 `CAPTURE + CLAIM`**：在 `GtkToggleButton` 内部放置可点击子控件（如箭头图标）时，子控件手势默认在 BUBBLE 阶段触发，外层 toggle 仍会收到按下并翻转。必须设 `GTK_PHASE_CAPTURE` 并调用 `gtk_gesture_set_state(..., GTK_EVENT_SEQUENCE_CLAIMED)`。
- **GTK4 已移除 `gtk_widget_set_reactive`**：切勿照抄 GTK3 写法。
- **菜单锚点取外层胶囊**：计算二级菜单锚点矩形时应向上定位到外层 `GtkToggleButton` 再取 bounds，否则点击胶囊其他区域会被误判为“菜单外点击”而立刻关闭刚打开的菜单。
- **所有权与生命周期**：
  - 弹层被挂入窗口后由窗口容器管理引用，调用方不得再额外 `g_object_unref`；
  - 传递数组出参使用「指针 + 显式 count」（不用三重指针或哨兵启发式）；
  - 菜单回调所需上下文或条目数据应堆分配并绑定到菜单/面板销毁回调（如 `g_object_set_data_full`），避免传栈变量地址或 `static` 局部数组引起悬空指针或不可重入问题。
- **自定义 GObject 内嵌 C++ 成员必须显式 placement-new 与析构**：`g_object_new()` 只做零初始化（zero-init），不会调用 C++ 字段（如 `std::vector`、`std::string`）的构造函数或析构函数；必须在 `*_init()` 中对每个非平凡成员执行 placement `new`，并在 `*_finalize()` 中显式调用析构函数再串联 `parent_class->finalize`（参见 `app-grid.cpp` 与 `overview-dash.cpp`）。
- **WinRT `UserNotificationListener` 与无打包桌面策略 / `wpnapps.dll` 缺 `AddRef` 陷阱**：`winrt-bridge.cpp` 在独立 MTA 工作线程通过 `combase.dll` 动态加载并按裸 vtable 调用 WinRT 接口（遵循同款 GCC LTO 去虚拟化约束）；非商店打包宿主可能被系统策略拒绝通知监听访问，调用方必须检查 `access_allowed` 并静默降级，不得阻塞 UI。此外，Win11（Build 22621）`wpnapps.dll` 中 `NotificationVisual::get_Bindings`（vtable slot 8）直接返回内部 `m_bindings` 指针而未调用 `AddRef()`，若直接放入所有权型 `ComPtr` 会提前将引用计数释放归零并在释放 `IAsyncOperation` 时触发 `wpnapps.dll+0xA7B09` UAF / `0xC0000374` 堆损坏；已通过 `winrt_acquire_com_getter_safe()` 二次探测并在未递增引用计数时补一次 `AddRef()` 规避，同时兼顾后续已修复系统版本避免泄漏。
- **`WH_KEYBOARD_LL` 吞键后状态查询与原子序贯透传**：`WH_KEYBOARD_LL` 吞掉 `VK_LWIN`/`VK_RWIN` 后，`win32k` 不会置位 `GetAsyncKeyState(g_win_vk)`，因此在回注 `Win` 之前切忌用 `GetAsyncKeyState` 判定“Win 已松开”（否则按住 `Win` 超过数十毫秒即误重置导致 `Win+Tab`/`Win+V` 失效）；透传不冲突 `Win+<key>` 时还必须在单次 `SendInput(2, ...)` 中原子注入 `[Win DOWN, 组合键 DOWN]` 并吞掉原乱序 `keydown`，且纯修饰键（`Shift`/`Ctrl`/`Alt`）延迟到非修饰触发键才回注 `Win`，保证 `Win+V`、`Win+Shift+S`、`Win+Ctrl+Left/Right` 与 `Win+Shift+PageUp` 互不干扰。

---

## 4. 样式复刻与排查方法论

1. **做浮层前先查主题 CSS**：[gnome-shell-dark.css](../WINOME/src/theme/gnome-shell-dark.css) 已自带 `.popup-menu`、`.quick-toggle-menu`、`.popup-menu-item`、`.header` 等完整规则，通常只需对齐上游 JS 节点树与类名，无需手写补充 CSS。
2. **优先静态对照查证**：比对上游 `.scss` + 上游 `.js` 节点结构 + 产物 `gnome-shell-gtk4.css`，比运行时盲探更准确。
3. **验证探针有效性**：切勿用 `gtk_style_context_lookup_color` 检测 `:checked` 等状态伪类——它在标准对照组（`GtkToggleButton` + `button.quick-toggle:checked`）上同样返回失败。当探针在对照组上失效时，应立即否定探针而非据此修改业务逻辑。
4. **验证构建期检查器有效性**：每次为 `check-st-css-coverage.py` 或 `check-widget-classes.py` 添加规则时，必须通过**人为注入已知缺陷**确认脚本确实报错；仅靠“绿灯通过”不能证明检查有效，同时注意增量构建不会重跑未修改的规则，必要时删除产物强行重编。
5. **GUI exe 输出行为**：由于工程启用了 `win_subsystem: 'windows'`，在 bash 下直接运行无法把 stdout 落盘到控制台，诊断输出默认按 `5MB` 上限轮转追加写入 `%LOCALAPPDATA%\WINOME\winome.log`（旧日志保留为 `winome.prev.log`）。

---

## 5. WinRT 桥接与设备环境说明

- **蓝牙 / SMTC / 通知真实数据源（已落地）**：针对 UCRT64 缺少 `ucrt64/include/winrt` 头文件的问题，`winrt-bridge.cpp` 通过运行时加载 `combase.dll`（`RoGetActivationFactory` / `WindowsCreateString`）与裸 COM/WinRT vtable 指针在后台 MTA 线程访问 `Windows.Devices.Radios.Radio`、SMTC 与 `UserNotificationListener`，无需引入 C++/WinRT 编译依赖即可工作；若系统策略禁用未打包进程访问通知监听，按 `access_allowed == false` 安静降级。
- **无线与硬件可选依赖**：Wi-Fi 枚举与连接依赖目标主机存在真实无线网卡及系统 `wlanapi.dll`，蓝牙依赖蓝牙适配器与系统 Radio 接口；无相应硬件的台式机/虚拟机将按能力探针静默降级，不影响其余界面功能。
