# WinOverview — 概览实现代码提取

本目录是从 [WinOverview](https://github.com/…) 项目中提取的 **GNOME Activities（概览）** 实现代码，
仅包含源代码文件（.cpp / .h），不含 `.rc`、`resource.h`、`.vcxproj`、`.sln` 等构建脚手架。

## 项目简介

WinOverview 是 GNOME Activities（Overview / 概览）在 Microsoft Windows 上的复刻实现：

- 按 Win 键（全局钩子监听）打开概览，实时显示各监视器上所有正在运行的窗口
- 窗口以缩略图网格展示（布局算法直接移植自 GNOME `workspace.js`）
- 点击缩略图将该窗口带回前台；点击空白处 / Esc 关闭概览
- 输入字母自动进入 Windows 搜索

## 目录结构与文件映射

```
extracted/WinOverview/
├── WinOverviewLibrary/            # 概览渲染核心（注入 RuntimeBroker.exe 的 DLL）
│   ├── dllmain.cpp                # 概览窗口生命周期、交互、搜索、点击处理
│   ├── workspace.cpp / .h         # 从 GNOME workspace.js 移植的窗口布局算法
│   ├── helpers.cpp / .h           # 监视器/窗口枚举、壁纸获取、DWM 缩略图、动画引擎
│   ├── structs.h                  # MonitorInfo / AnimationInfo / WindowInfo 等数据结构
│   ├── constants.h                # 布局/动画常量、窗口类名、自定义 WM 消息
│   └── framework.h / pch.h / pch.cpp
├── WinOverview/                   # 守护进程（概览触发入口，WinOverview.exe）
│   ├── WinOverview.cpp            # SetWindowsHookEx 低层键盘/鼠标钩子，Win 键触发
│   └── NtUserBuildHwndList.h      # 未文档化 API：枚举含 Metro/沉浸式应用的全部窗口
└── WinOverviewLauncher/           # 注入启动器（WinOverviewLauncher.exe）
    └── WinOverviewLauncher.cpp    # 拉起 RuntimeBroker.exe 并注入 DLL、调用 main 导出
```

> 目录结构与原始仓库保持一致（`constants.h` 被守护进程与 DLL 共用，相对路径不可改动）。

## 组件协作流程

```
┌─────────────────┐    Win 键    ┌──────────────────────────┐
│ WinOverview.exe │─────────────▶│ 在 explorer.exe 内 WinExec│
│  (守护进程)      │              │ 启动 WinOverviewLauncher  │
└─────────────────┘              └────────────┬─────────────┘
                                              ▼
┌─────────────────┐   注入 DLL     ┌──────────────────────────┐
│ RuntimeBroker   │◀──────────────│ WinOverviewLauncher.exe  │
│  (宿主进程)      │  CreateRemote │  拉起 RuntimeBroker + Job │
└────────┬────────┘  Thread        │  KILL_ON_JOB_CLOSE        │
         ▼                         └──────────────────────────┘
┌─────────────────┐
│ WinOverview.dll │  main() 导出：创建概览窗口（CreateWindowInBand）→ 渲染缩略图 → 消息循环
└─────────────────┘
```

概览关闭后，Job 对象自动终止 RuntimeBroker 与 Launcher，只留下守护进程继续监听。

## 编译

提取目录不含工程文件，可参照原项目的方法重新构建三个工程：

1. **WinOverview.dll**（库）—— C++ DLL，导出 `main`（注：为普通导出函数，非 DllMain）
2. **WinOverviewLauncher.exe** —— 普通 exe，链接 `shlwapi.lib`、`Psapi.lib` 等
3. **WinOverview.exe**（守护进程）—— 普通 exe，链接 `shlwapi.lib` 等

原项目使用 Visual Studio 2019（Build Tools v16，Windows 10 SDK 10.0.18362.0）。
原 README 给出的命令行编译示例（对应旧的单文件 Overview）：

```bat
rc /nologo Overview.rc
cl /nologo /DUNICODE Overview.cpp workspace.cpp /FeOverview.exe /link Overview.res user32.lib gdi32.lib ole32.lib dwmapi.lib
```

## 运行要求

- 守护进程需以 **管理员权限** 运行，才能在所有应用（含提权应用）中监听激活按键
- 开机自启：使用任务计划程序创建"登录时启动"，勾选"使用最高权限运行"
- 概览渲染需要 DWM（桌面窗口管理器），即 Aero 合成开启
- 开发与测试环境为 Windows 10

## 主要依赖的（未）文档化 API

| API | 用途 | 来源 |
|---|---|---|
| `SetWindowsHookEx` | 全局低层键盘/鼠标钩子 | winuser.h |
| `CreateWindowInBand` | 在桌面顶层 band 创建概览窗口 | user32.dll（未文档化） |
| `NtUserBuildHwndList` | 枚举含 Metro 应用的全部窗口 | win32u.dll（未文档化） |
| `DwmRegisterThumbnail` / `DwmUpdateThumbnailProperties` | 实时窗口缩略图 | dwmapi.h |
| `SetWindowCompositionAttribute` | 背景模糊（代码中已注释） | user32.dll（未文档化） |
| `BitBlt` | 从桌面 hWnd 拷贝壁纸作为背景 | wingdi.h |

## 已知问题（来自原项目）

- 点击搜索框外侧时，点击事件会穿透到下方窗口
- 高 CPU 负载下动画可能掉帧（动画线程使用定时器插值实现）

更多实现细节见 [ARCHITECTURE.md](ARCHITECTURE.md)。
