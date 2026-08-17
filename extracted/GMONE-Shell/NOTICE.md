# NOTICE — 修改声明

本目录的代码与资源派生自 [GNOME Shell](https://github.com/GNOME/gnome-shell)
（GPL-2.0-or-later，版本 51.beta，提交 `a23b436f`），遵循 GPL-2.0-or-later 条款发布。
依据 GPL-2.0 §9 "or any later version" 条款，WINOME 以 GPL-2.0 使用本目录代码。

## 修改内容

本目录从 gnome-shell 提取了「Shell 外观主题」与「纯布局/日期/动画算法」，并进行了以下修改：

1. **移除不可移植部分**：仅保留 `data/theme/` 主题资源与 `js/` 中可移植的纯逻辑
   文件，未包含依赖 St/Clutter/Meta/GJS 的 UI 实现、C 源码、D-Bus 服务、翻译与测试。
2. **添加文件头**：每个文件顶部统一添加 SPDX 标识、来源与修改日期注释
   （`GPL-2.0-or-later`）。
3. **保留原始内容**：除文件头外，源文件内容与上游保持一致，未作其他改动。
4. **保留原版权声明**：`theme/gnome-shell-sass/COPYING`（GPL-2.0）随目录保留，
   原始版权归属 FSF 及各贡献者。

## 修改日期

2026-08-16

## 原始许可

原项目许可为 GPL-2.0-or-later，全文见上游仓库 `COPYING` 或本仓库根目录
`gnome-shell/COPYING` 与 `THIRD-PARTY-NOTICES`。
