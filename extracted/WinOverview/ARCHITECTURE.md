# 架构与实现原理

本文档详解 WinOverview 概览的实现机制，对应提取后的三个组件。

## 1. 守护进程（WinOverview / WinOverview.cpp）

职责：**监听全局按键，触发概览；概览运行期间转发系统级事件**。

### 按键监听
- 通过 `SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0)` 安装低层键盘钩子（需要管理员权限，否则无法拦截提权进程的按键）
- `WM_KEYDOWN` 时记录 Win 键（`VK_LWIN`/`VK_RWIN`），其余按键清零 `lastKey`；`WM_KEYUP` 时判断：
  - Win 键抬起且概览未运行 → `CreateThread(run)` 启动概览，并安装 `WH_MOUSE_LL` 鼠标钩子
  - Escape / 再次按 Win → 向所有概览窗口发送 `WM_CLOSE`
  - Enter 且概览运行 → 向概览窗口发送 `WM_CLOSE`（携带参数触发搜索确认）
- 同时向系统发送一次 `Ctrl` 键按下/抬起，规避 Windows 对"按 Win 键后按键的拦截"

### 触发方式（`run` 线程）
- 遍历进程快照（`CreateToolhelp32Snapshot`）找到 `explorer.exe`
- 用 `VirtualAllocEx` + `WriteProcessMemory` 把启动命令写入 explorer 进程
- `CreateRemoteThread` 调用 explorer 内的 `Kernel32!WinExec` 启动 `WinOverviewLauncher.exe`
  - **必须用 WinExec 在 explorer 中启动**：RuntimeBroker.exe 无法从管理员进程直接拉起，且让概览保持普通权限更安全

### 窗口通信
- 概览窗口所在进程（RuntimeBroker）是沉浸式进程，`EnumWindows` 枚举不到
- 故从 `win32u.dll` 动态加载 `NtUserBuildHwndList`（见 NtUserBuildHwndList.h），枚举全部顶层窗口（含 Metro 应用），按类名 `CLASS_NAME`（`ActivitiesOverviewWindowClassFull`）识别概览窗口并发送消息：
  - `WM_CLOSE`：关闭概览
  - `WM_ASK_MOUSE`：把鼠标位置转发给概览窗口，用于点击判定（点击空白处关闭、点击窗口带回前台）

## 2. 注入启动器（WinOverviewLauncher / WinOverviewLauncher.cpp）

职责：**拉起宿主进程并注入渲染 DLL**。

1. `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` 保证 DPI 感知
2. 用 `NtUserBuildHwndList` 检查概览是否已在运行（类名匹配 `CLASS_NAME`），已在则直接退出
3. 用 `CreateProcess` 启动 `C:\Windows\System32\RuntimeBroker.exe`
4. 创建 Job 对象（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`）并把 RuntimeBroker 挂进去：
   概览窗口关闭 → Job 无句柄 → 自动终止整个进程树，无需显式清理
5. DLL 注入：
   - `VirtualAllocEx` + `WriteProcessMemory` 写入 DLL 路径
   - `CreateRemoteThread` 调用 `Kernel32!LoadLibraryW` 完成加载
   - 本进程 `LoadLibrary(szLibPath)` 后 `GetProcAddress("main")` 拿到函数地址
   - 用 `EnumProcessModules` 找出 DLL 在宿主进程中的基址，按
     `hMods[i] + hInjectionMainFunc - hInjectionDll` 计算宿主内的函数地址
   - 再 `CreateRemoteThread` 调用该 `main` 导出（线程入口运行概览）
6. `WaitForSingleObject(processInfo.hProcess, INFINITE)` 阻塞至概览结束

## 3. 渲染 DLL（WinOverviewLibrary）

`dllmain.cpp` 的 `main` 导出是概览主入口，流程：

### 3.1 窗口创建（置顶显示的关键）
- 注册三个窗口类：`CLASS_NAME`（主窗口）、`CLASS_NAME_SIMPLE`（辅助）、`CLASS_NAME_BKG`（搜索背景）
- 从 `user32.dll` 动态加载未文档化的 `CreateWindowInBand` / `GetWindowBand` / `SetWindowBand`
- `CreateWindowInBand(WS_EX_TOPMOST | WS_EX_LAYERED, ..., ZBID_LOCK)`：
  把概览窗口放入 **ZBID_LOCK 带**（位于桌面 z-order 最顶层，锁屏层之上），从而盖过所有应用窗口
- `SetLayeredWindowAttributes` 设置不透明度；`SetWindowCompositionAttribute(ACCENT_ENABLE_BLURBEHIND)`
  可启用背景模糊（代码中已注释）

### 3.2 数据收集（helpers.cpp）
- `GetMonitors`：`EnumDisplayMonitors` 枚举所有监视器，归一化工作区坐标（`realArea`/`area`），
  并计算多显示器虚拟桌面左上角偏移（`wallpaperOffset`）
- `GetWindowsOnMonitor`：`EnumWindows` + 过滤条件（helpers.cpp `EnumWindowsProc`）：
  - `IsAltTabWindow`（来自 Old New Thing 的 Alt-Tab 判定：可见、无 `WS_EX_TOOLWINDOW`、活动弹窗归属）
  - `DwmGetWindowAttribute(DWMWA_CLOAKED)` 为假（排除已挂起的 UWP 应用）
  - 非最小化（`!IsIconic`）、属于当前监视器（`MonitorFromWindow`）
- `GetWallpaperWindows`：向 `Progman` 发送 `0x052C` 消息让其创建 `WorkerW` 桌面层，
  `EnumWindows` + `GetWallpaperHwnd` 找到 `WorkerW` 句柄 → 用于 BitBlt 壁纸背景

### 3.3 布局计算（workspace.cpp，移植自 GNOME workspace.js）
`GetWindowSlots` 在 1..n 行间迭代，对每个候选布局执行：
1. `computeLayout`：按窗口中心点纵向排序后贪心分行（`keepSameRow` 以理想行宽为准），
   行内再横向排序；得到网格宽高、最大列数
2. `computeScaleAndSpace`：按可用区域计算缩放 `scale` 与空间占比 `space`
3. `isBetterLayout`：用 `LAYOUT_SCALE_WEIGHT` / `LAYOUT_SPACE_WEIGHT` 权衡保留更优布局
4. `computeWindowSlots`：按行/列间距、垂直/水平额外缩放、像素对齐（`floor`）计算每个窗口缩略图的最终位置与缩放

### 3.4 缩略图与动画
- `RegisterLiveThumbnail`：`DwmRegisterThumbnail` + `DwmUpdateThumbnailProperties`
  （DWM 直接把源窗口绘制到目标窗口指定矩形，实时跟随窗口内容）
- `animate` 线程：按 `FPS`（120）分帧插值，`easeOutQuad` 缓动
  - `ANIMTYPE_PREVIEW`：窗口从原始位置→网格槽位（位置+缩放）
  - `ANIMTYPE_PREVIEW_FADE`：缩略图透明度淡出
  - `ANIMTYPE_MAIN_FADE`：整个概览窗口 `SetLayeredWindowAttributes` 淡入淡出
- 线程用信号量 `hAnimMutex` 互斥（`BeginAnimateUpdate`/`EndAnimateUpdate`），
  完成后发 `WM_THREAD_DONE` 通知主窗口；关闭类动画结束后直接终止进程

### 3.5 交互
- **点击缩略图**：`WM_LBUTTONUP` 中命中检测各动画槽位 → 重新注册缩略图、
  `SetForegroundWindow` 带回前台、`DoAnimate(PREVIEW)` 收缩动画后关闭
- **点击空白**：关闭概览并恢复 `previous`（打开前的焦点窗口）
- **搜索**：`WM_KEYDOWN` 收到字母/数字 → 创建 `CLASS_NAME_BKG` 背景窗，
  `ShowSearch` 用 `SendInput` 触发 `Win+Q` 打开 Windows 搜索，再回放用户键入的字符；
  搜索窗口 `hSearchHWnd` 被移动到概览中央（`SetWindowPos`）；通过 `WM_ASK_MOUSE`
  判定鼠标是否移出搜索框/概览区域来决定是否退出搜索
- **Esc / 焦点丢失**：`WM_KEYUP(VK_ESCAPE)` 或 `DetectSearchDismiss`（前台事件钩子）关闭

## 4. 关键常量（constants.h）

```cpp
#define _columnSpacing 20;  _rowSpacing 20;        // 网格行列间距
#define WINDOW_CLONE_MAXIMUM_SCALE 1.0             // 缩略图最大 100%
#define ANIMATION_DURATION_MS 150   FADE_DURATION_MS 120
#define FPS 120                                     // 动画帧率
#define AREA_BORDER 30                              // 概览内容四周留白
CLASS_NAME / CLASS_NAME_SIMPLE / CLASS_NAME_BKG    // 三个窗口类名（进程间识别依据）
WM_THREAD_DONE / WM_ASK_MOUSE / WM_SHOW_SEARCH / WM_IS_SEARCH  // 自定义消息
```

## 5. 已知缺陷与后续方向

- 搜索框外点击会穿透到底层窗口（WM_NCHITTEST 相关逻辑被注释）
- 动画插值用 `Sleep`/定时器，CPU 高负载时易掉帧
- 多处强类型假定（monitor 数组按顺序、`uIndex` 未初始化路径等）需要加固
- 大量依赖未文档化 API（`CreateWindowInBand`、`NtUserBuildHwndList`），Windows 版本演进可能破坏行为
