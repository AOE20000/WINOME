# GNOME Shell 提取 — Shell 外观与布局算法

本目录从 [GNOME Shell](https://github.com/GNOME/gnome-shell)（GPL-2.0-or-later）中提取
**GNOME Shell 的视觉主题与纯逻辑算法**，作为 WINOME 在 Windows 上以 C++/GTK4 复刻
GNOME Shell UI 的参考来源。

这些代码本身**不在 Windows 上运行**，而是作为移植素材：Sass 主题编译为 GTK4 CSS，
JS 中的布局/日期/动画算法以 C++ 重写。

## 目录结构

```
extracted/GMONE-Shell/
├── theme/                        # GNOME Shell 视觉主题
│   ├── gnome-shell-sass/         # Sass 框架（颜色、绘制、widget 样式）
│   │   ├── _colors.scss          #   基础颜色变量
│   │   ├── _palette.scss         #   GNOME 调色板
│   │   ├── _drawing.scss         #   绘制工具（按钮/进度/滑块等）
│   │   ├── _common.scss / _widgets.scss
│   │   └── widgets/              #   33 个控件样式（panel/dash/app-grid/...）
│   ├── gnome-shell-dark.scss / light.scss / high-contrast.scss
│   ├── pad-osd.css
│   └── *.svg                     # 占位符/指示器图标（6 个）
└── js/                           # 纯逻辑算法（JS 源，供 C++ 移植参考）
    ├── ui/
    │   ├── iconGrid.js           # 应用网格布局（行列/spacing/分页）
    │   ├── appDisplay.js         # 应用网格翻页/重排
    │   ├── dash.js               # Dock 图标自动缩放
    │   ├── overviewControls.js   # 概览布局（搜索/Dash/缩略图高度分配）
    │   ├── calendar.js           # 日期数学（工作日/日首日尾）
    │   ├── panel.js              # 面板结构（左中右 Box 布局）
    │   ├── quickSettings.js      # 快速设置结构参考
    │   ├── workspace.js          # 窗口缩略图布局（与 WinOverview 提取同源）
    │   ├── altTab.js             # Alt-Tab 切换器结构参考
    │   └── environment.js        # 动画缓动辅助
    └── misc/
        ├── dateUtils.js          # CLDR 日期/时间格式化
        ├── animationUtils.js     # 动画时间/缓动参数
        ├── util.js               # 通用工具（lerp 等）
        ├── params.js             # 参数解析
        └── dbusUtils.js          # D-Bus 接口加载
```

## 用途映射

| 提取内容 | WINOME 用途 |
|---------|-------------|
| `theme/gnome-shell-sass/widgets/_panel.scss` | 顶部面板 GTK4 CSS |
| `theme/gnome-shell-sass/widgets/_dash.scss` | Dock/Dash GTK4 CSS |
| `theme/gnome-shell-sass/widgets/_app-grid.scss` | 应用网格 GTK4 CSS |
| `theme/gnome-shell-sass/widgets/_quick-settings.scss` | 快速设置 GTK4 CSS |
| `theme/gnome-shell-sass/_colors.scss` / `_palette.scss` | 颜色设计令牌 |
| `js/ui/iconGrid.js` | 应用网格布局算法 → C++ |
| `js/ui/dash.js` | Dock 缩放算法 → C++ |
| `js/ui/overviewControls.js` | 概览高度分配 → C++ |
| `js/ui/workspace.js` | 窗口缩略图布局（参考 WinOverview）→ C++ |
| `js/misc/dateUtils.js` | 时钟/日历格式化 → C++ |
| `js/misc/animationUtils.js` + `environment.js` | 动画缓动 → C++ |
| `js/ui/calendar.js` | 日历日期数学 → C++ |
| `theme/*.svg` | 占位符/指示器图标 → GTK4 |

## 许可

整体遵循 GPL-2.0-or-later（详见 WINOME 根目录 `LICENSE` 与 `THIRD-PARTY-NOTICES`）。
所有文件已添加 SPDX 头与来源/修改声明；更多修改记录见 [NOTICE](NOTICE.md)。
