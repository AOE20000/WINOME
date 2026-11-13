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
10. **启动关键路径避开 COM**：VD 首次枚举延后 2s、壳层窗口 pin 延后 3s，防止低完整性下 explorer RPC 死锁卡住启动。

---

## 2. Win32 API 踩坑

- **按需系统 DLL 必须动态加载**：`wlanapi.dll` 在无无线网卡的机器上不存在，静态链接会导致进程无法启动；用 `LoadLibraryW` + `GetProcAddress` 解析入口并做静默降级。
- **`GetProcAddress` 转换消除 `-Wcast-function-type`**：将 `FARPROC` 先转为 `void*` 再 `reinterpret_cast` 为目标函数指针，消除编译器警告。
- **动态解析签名无编译期校验**：`WlanOpenHandle` 只有 **4 个参数** `(DWORD, PVOID, PDWORD, PHANDLE)`；`WlanGetAvailableNetworkList` 第三参是保留位必须传 `0`（无 `*_ALL` 常量），出参为 `PWLAN_AVAILABLE_NETWORK_LIST`。签名或参数顺位写错不会在链接期报错，只会在运行时返回异常错误码。
- **802.11 SSID 原始字节解码**：`ucSSID` 不是 NUL 结尾文本，`uSSIDLength` 是字节数；中文 SSID 常为 UTF-8（4 个汉字占 12 字节），也有 Latin-1。应先校验 UTF-8，失败再按 Latin-1 展开，并预留 `97` 字节缓冲区（32 字节最坏展开 96 字节 + NUL）。
- **WLAN 同名 BSSID 去重与扫描缓存**：Windows 按 BSSID 返回多行，需按 SSID 合并保留最强信号；可见网络列表受 Windows 驱动扫描缓存影响，有时仅返回当前 AP 的若干 BSSID，每次打开菜单重新扫描即可。
- **PowrProf 方案 GUID 与头文件**：节能 `a1841308…`、均衡 `381b4222…`（Win11 另有随机化均衡 GUID `b9a9a03a…`）、性能 `8c5e7fda…`；`PowerEnumerate` 首参 `HKEY` 传 `NULL`、枚举标志用 `ACCESS_SCHEME`；`PowerGetActiveScheme(HKEY, GUID**)` 位于 `powersetting.h`，返回的 GUID 需 `LocalFree`。
- **检查 Win32 `BOOL` 返回值**：`ExitWindowsEx`、`SetSuspendState` 等操作受会话权限与电源策略限制，务必检查返回状态。

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

---

## 4. 样式复刻与排查方法论

1. **做浮层前先查主题 CSS**：[gnome-shell-dark.css](../WINOME/src/theme/gnome-shell-dark.css) 已自带 `.popup-menu`、`.quick-toggle-menu`、`.popup-menu-item`、`.header` 等完整规则，通常只需对齐上游 JS 节点树与类名，无需手写补充 CSS。
2. **优先静态对照查证**：比对上游 `.scss` + 上游 `.js` 节点结构 + 产物 `gnome-shell-gtk4.css`，比运行时盲探更准确。
3. **验证探针有效性**：切勿用 `gtk_style_context_lookup_color` 检测 `:checked` 等状态伪类——它在标准对照组（`GtkToggleButton` + `button.quick-toggle:checked`）上同样返回失败。当探针在对照组上失效时，应立即否定探针而非据此修改业务逻辑。
4. **验证构建期检查器有效性**：每次为 `check-st-css-coverage.py` 或 `check-widget-classes.py` 添加规则时，必须通过**人为注入已知缺陷**确认脚本确实报错；仅靠“绿灯通过”不能证明检查有效，同时注意增量构建不会重跑未修改的规则，必要时删除产物强行重编。
5. **GUI exe 输出行为**：由于工程启用了 `win_subsystem: 'windows'`，在 bash 下直接运行无法把 stdout 落盘到控制台，诊断输出默认追加写入 `%TEMP%\winome.log`。

---

## 5. 明确暂缓项与本机环境事实

- **蓝牙二级菜单与真实数据源（暂缓）**：MinGW / UCRT64 工具链不包含 WinRT 头文件（`ucrt64/include/winrt` 不存在），而 Windows 枚举蓝牙 radio 与发现设备需依赖 WinRT `Windows.Devices.Bluetooth` API；`bluetoothapis.h` 仅面向已配对设备且无法控制 radio 开关。待确定独立后端桥接方案后再行接入，不做虚假 stub。
- **本机无线调试环境**：本机具备真实无线网卡（Intel Dual Band Wireless-AC 7265），可在实机验证 `wlanapi.dll` 扫描与连接流程。
