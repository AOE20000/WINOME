# WINOME 主工程

本目录是 WINOME 的 Meson 构建根目录（C++17 宿主进程 + GTK4 UI + St CSS 引擎复刻）。

> 项目功能状态清单与快捷键说明统一维护在仓库根目录 [README.md](../README.md)；完整设计、环境与踩坑文档请查阅 [docs/README.md](../docs/README.md)。

## 目录结构

| 路径 | 说明 |
|------|------|
| `src/` | 宿主进程与各 UI / Win32 集成模块（产出 `winome.exe`） |
| `src/theme/` | 上游 `gnome-shell-dark.css` 真源、Cantarell 字体、12 个内置 GNOME symbolic SVG 图标、日历/工作区占位 SVG 及 GResource 配置 |
| `st-compat/` | 提取自 GNOME Shell / libcroco 的 St CSS 解析与属性求值引擎（去 Clutter/Cogl） |
| `tools/` | St → GTK4 样式转换器（`convert-st-css.py`）、`st-css-eval` 及覆盖率/类名自检脚本 |
| `tests/` | 单元测试（`test-main.cpp`）与 CSS 转换器回归测试（`test-css-converter.py`） |
| `meson.build` / `meson.options` | Meson 构建配置 |
| `LICENSE` / `THIRD-PARTY-NOTICES` | 许可证与上游第三方声明 |

## 构建

推荐使用仓库根目录的构建脚本（详见 [docs/build-and-env.md](../docs/build-and-env.md)）：

```bash
../build.sh            # 交互式菜单（显示各方案产物状态）
../build.sh release    # 生产：release + LTO + NDEBUG -> build-release/
../build.sh test       # 测试：debugoptimized (默认)   -> build/
../build.sh debug      # 调试：debug -O0 -g            -> build-debug/
../build.sh run        # 运行最近构建的 winome.exe
../build.sh clean      # 清理全部构建目录
```

手工调用 Meson / Ninja 前请注意在 MSYS2 UCRT64 下设置 ASCII 临时目录，并在重编前清理陈旧的 `st-compat/libstcompat.a`（详见 [docs/build-and-env.md](../docs/build-and-env.md)）。

## 文档延伸阅读

- [系统架构与各子系统实现](../docs/architecture.md)
- [双 CSS 主题管线、动效层与快速设置规范](../docs/theme-and-ui.md)
- [硬约束、Win32/GTK4 踩坑与开发约定](../docs/pitfalls.md)

## 权限与日志

- 隐藏原生任务栏、操作 Explorer 窗口与完整访问虚拟桌面 COM 接口需要**管理员权限**运行 `winome.exe`（未提权时虚拟桌面模块在 8s 超时后自动降级为单工作区，不崩溃、不冻结）。
- 宿主以 GUI 子系统构建（无终端窗口），诊断输出按 5MB 上限轮转写入 `%LOCALAPPDATA%\WINOME\winome.log`（旧日志滚动保留为 `winome.prev.log`）。

## 许可

GPL-2.0，详见 `LICENSE` 与 `THIRD-PARTY-NOTICES`。
