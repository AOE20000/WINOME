# 主题管线、St 引擎与 UI 复刻规范

WINOME 以 sassc 编译后的上游样式表 [gnome-shell-dark.css](../WINOME/src/theme/gnome-shell-dark.css) 作为唯一视觉真源，通过“运行时 St 查询 + 构建期 GTK4 转换”双管线驱动 UI。

---

## 1. 双 CSS 管线

两份样式表通过 [winome.gresource.xml](../WINOME/src/theme/winome.gresource.xml) 一并打入 GResource（前缀 `/org/winome/theme`）：

1. **原始版 `gnome-shell-dark.css`** → 供运行时 St 引擎查询；
2. **转换版 `gnome-shell-gtk4.css`** → 由 [convert-st-css.py](../WINOME/tools/convert-st-css.py) 生成，供 GTK4 `GtkCssProvider` 加载（0 解析报错）。

### 运行时 St CSS 引擎（`WINOME/st-compat/` + `src/st-engine.{h,cpp}`）

完整抽取 GNOME 的 libcroco + `st-theme-node` 样式求值引擎（去除 Clutter / Cogl 依赖），在运行时精确查询无法直接由 GTK4 CSS 表达的 St 专有属性（如 `icon-size`、`spacing`、`-natural-hpadding`、em 换算与父链级联继承）：

| 组件 | 来源 | 用途 |
|------|------|------|
| `st-compat/croco/` | libcroco | CSS 词法与语法解析（21 个 `.c` 文件，纯 GLib） |
| `st-theme.c` / `st-theme-node.c` | gnome-shell | 样式表加载、级联与属性求值（4565 行核心） |
| `st-shadow.c` / `st-border-image.c` / `st-icon-colors.c` | gnome-shell | St 派生数据类型 |
| `st-compat.h` / `st-compat-impl.c` | WINOME | `CoglColor` / `ClutterActorBox` shim 与极简 `StThemeContext` / `StSettings` |
| `src/st-engine.{h,cpp}` | WINOME | C++ 封装，支持复合选择器查询（例如 `#panel .panel-button .system-status-icon`） |

---

## 2. St → GTK4 转换规则与自检

### 核心转换规则（`tools/convert-st-css.py`）

| GNOME (St) | GTK4 等价处理 | 说明 |
|------------|--------------|------|
| `st-mix()` / `st-lighten()` / `st-darken()` / `st-transparentize()` | 委托 `st-css-eval` | 链接 `st-compat` 执行 St 原生 C 颜色数学，保证与 GNOME 逐字节一致，不在 Python 中重写 |
| `-st-accent-color` / `-st-accent-fg-color` | 委托 `st-css-eval` | 解析为 `#3584e4` / `#ffffff` |
| `box-shadow: inset 0 0 0 100px <c>` | 改写为 `background-color: <c>` | 规避 GTK4 大 spread 内阴影渲染 bug |
| `width` / `height` | `min-width` / `min-height` | St 的 `width`/`height` 语义为下限 |
| `max-width` / `max-height` | `max-width` / `max-height` | **必须单独映射到 `max-*`**；若误映射到 `min-*` 会丢失固定胶囊上限（例如 `.quick-toggle` 固定 12em 网格会垮掉） |
| `-natural-hpadding` | `padding-left` + `padding-right` | 展开为两条声明 |
| `:insensitive` | `:disabled` | 伪类重映射 |
| `:overview` / `:unlock-screen` / `:rtl` 等 | 整条规则丢弃 | 避免剥离伪类后误生效到默认态 |
| `:ltr` 与逗号分组中的可保留子选择器 | 按子选择器粒度过滤 | `_clean_subselector()` 与多行选择器续行保留有效 `:ltr` 与同组其余选择器 |
| `!important` | 剥离 | GTK4 对 `box-shadow` 等不支持 |
| 省略 `transition-property` 的计时规则 | `inject_transition_property()` 后处理补全 | St 省略时默认 `background-color`，GTK4 无默认值会导致 hover 硬跳变；按块补 `background-color, color, border-color, box-shadow` |

> ⚠️ 修改 `convert-st-css.py` 时的原则：`inject_transition_property()` 必须保持**纯文本输出后处理**，不要重构主解析循环；同时识别独立 `}` 行和挂在最后一条声明上的闭合括号（如 `padding: 9px 12px; }`），改动后需用 `diff` 确认只有新增行。

### 构建期与结构自检工具

1. **样式属性覆盖率门禁**（[check-st-css-coverage.py](../WINOME/tools/check-st-css-coverage.py)，经 [convert-then-check.py](../WINOME/tools/convert-then-check.py) 接入 Meson 构建步骤，失败即中止构建）：
   - `coverage`：上游属性必须全部落在 `RENAME` / `EXPAND` / `DROP` 或显式 `GTK4_NATIVE` 中（三张表直接正则读取 `convert-st-css.py`，禁止隐式透传）；
   - `pass-through`：声明为 GTK4 原生的属性必须真正进入输出；
   - `semantics`：同时设定 `min-width` + `max-width`（或 `min-height` + `max-height`）的规则不可丢失上限；
   - `transitions`：输出中所有带 `transition-duration` 的规则数不得超过带 `transition-property` 的规则数（采用**数量比较**而非存在性检查，防止面板硬编码或残留规则掩盖回归）。
   - 当前基线：`OK 83 upstream properties all accounted for; 40 reached the GTK4 stylesheet.`
2. **控件类名结构检查**（[check-widget-classes.py](../WINOME/tools/check-widget-classes.py)）：
   - 交叉核对 `gnome-shell-gtk4.css` 中的作用域样式类与 C++ 源码中的 `gtk_widget_*_css_class` 调用，抓出「样式存在但代码未建节点」或「代码加了但样式表未定义的死类」。

---

## 3. Clutter 动效层（`src/easing.{h,cpp}`）

GTK4 没有内置动画框架（无 `GtkAnimation`），而上游广泛依赖 `Clutter.EaseFunction`：

- **曲线**：精确移植 Clutter `clutter-easing.c` 的 cubic-bezier 参数（默认 `ease_out_cubic = (0.215, 0.61, 0.355, 1.0)`；`ease_out_back = (0.34, 1.56, 0.64, 1.0)` 在 `t≈0.6` 过冲至 ~1.0966 后回落）。
- **求解**：Newton-Raphson + **二分法兜底**（`ease_out_back` 导数趋零会导致纯 Newton 停滞）。
- **驱动**：`gtk_widget_add_tick_callback` 对齐显示器 vblank，tween 状态随 widget 销毁释放。
- **上游时长常量**：`QUICK_SETTINGS_ANIMATION_TIME = 400`（`quickSettings.js:17`）、`BOXPOPOVER_ANIMATION_TIME = 150`（`boxpointer.js:14`）。
- **菜单两阶段串行开合**（`quickSettings.js:464 / :500`）：
  - open：先做高度补间，完成后淡入；
  - close：先淡出（`opacity → 0`），完成后收起高度并隐藏窗口。
  - **不能在关动画前调 `gtk_widget_set_visible(FALSE)`**，否则窗口掉出 topmost 队形且淡出不可见；完成回调仅持 window 引用，不碰已随窗口销毁释放的 `PanelPopover`。

---

## 4. 顶栏与快速设置结构移植要点

### 顶栏（`src/shell-panel.cpp`）

- **控件树**：Activities = `.panel-button#panelActivities` 内含自定义 `WinomeDot`（不用 `GtkBox`，否则会被 `#panel .panel-button#panelActivities box` 规则施加多余 padding）；时钟 = `.panel-button.clock-display` 内含 `.clock` label；状态区 = `.panel-button` 内含 `.panel-status-indicators-box` + 图标。
- **点击去聚焦**：所有面板按钮必须调用 `gtk_widget_set_focus_on_click(FALSE)`，否则 GTK4 点击后保留 `:focus` 状态导致高亮“粘住”。

### 快速设置网格与二级菜单（`src/quick-settings.cpp`）

- **网格顺序与 `colSpan`**（对齐 `panel.js:381-405`）：`N_QUICK_SETTINGS_COLUMNS = 2`。前 3 行（系统项、音量滑块、亮度滑块）占满 2 列；之后半行开关按网络、蓝牙、电源模式、夜间模式、深色主题、勿扰模式、键盘、飞行模式依次排列，维持固定 `12em` 胶囊视觉分组。
- **自绘滑块 `WinomeQuickSlider`**：无完整 GTK 主题环境下，`GtkScale` 会退化为 2×2px 且难以拖动，故以 `GtkDrawingArea` + 拖动/点击手势自绘 accent 进度条与圆形手柄。
- **`:checked` 状态误判排查结论**：早先曾怀疑 `:checked` 规则被转换器丢弃，后经源码与产物 CSS 对照确认 `.quick-toggle:checked { color: #fff; background-color: #3584e4 }` 与 `GTK_STATE_FLAG_CHECKED` 一切正常；此前“看着不像”的真正原因是二级菜单缺失 `.header` 以及 `max-width` 映射丢失。
- **二级菜单结构（`QuickToggleMenu`）**：
  - 顶部 `.header` 网格（`build_quick_menu_header()`）：24px `.icon` 圆形徽章（`999px` 圆角，激活加 `.active` 蓝色填充）+ `.title` + `.subtitle`；
  - 菜单容器自身携带 `.quick-toggle-menu`（`@extend %card`，`#47474c` 底、24px 圆角），行条目（`.popup-menu-item`）直接作为其子节点——**严禁再套 `.popup-menu-content` 或外层卡片**，否则出现双层圆角边框；
  - 胶囊内 `>` 箭头手势设为 `GTK_PHASE_CAPTURE` 并在 `pressed` 中 `gtk_gesture_set_state(..., GTK_EVENT_SEQUENCE_CLAIMED)`，阻止点击冒泡翻转外层 `GtkToggleButton`。
- **`BoxPointer._reposition()` 定位**（对齐 `boxpointer.js:480`）：
  ```text
  margin = 4 * arrow_border_radius + arrow_border_width + arrow_base  // 4*22 + 1 + 20 = 109
  resX   = anchorCenterX - (margin / 2 + (natWidth - margin) * 0.5)
  resX   = clamp(resX, workarea.x + arrow_rise, workarea.x + workarea.width - (arrow_rise + natWidth))
  resY   = anchorBottom + boxpointer_gap                              // + 4px
  ```
  不能简单按锚点右缘对齐，否则多出的 `margin/2`（约 54px）会让菜单整体左偏。


## 5. 全壳 UI 一致性管线（Phase 9–11 汇总）

- **CSS 转换恢复与 Adwaita 隔离（`tools/convert-st-css.py`）**：
  - `_clean_subselector()` 按逗号分组子选择器粒度保留 `:ltr` 与同类有效子选择器，修复 `main()` 多行选择器续行误丢弃问题（回归用例见 `tests/test-css-converter.py`）。
  - `GTK4_BUTTON_OVERRIDE` 补全 Adwaita 按钮样式重置、`.page-indicator` 重置、`switch.toggle-switch` 与 `> slider` 样式、`.quick-settings-popover-window` 与 `.winome-overview` 内嵌滚动条样式，以及 `.winome-overview popover.background`、`window#winomeToastWindow` 透明与 `.quick-toggle-menu { margin-top: 0; }`（保证 `BoxPointer` 箭头紧贴弹出面板）；`tools/check-widget-classes.py` 纳入 `toggle-switch` 可选前缀。
- **统一字体注入与控件状态对齐**：
  - 全部 8 处壳层顶层窗口安装点（`shell-panel.cpp`、`notifications.cpp`、`quick-settings.cpp` 密码弹窗、`settings-window.cpp`、`ws-osd.cpp` 与 `overview.cpp` 的 `g_window` / `g_chrome_window` / `g_icon_window`）统一调用 `winome::install_bundled_fonts()`，并移除 `overview-dash.cpp`（`kDashCtxCss`）、`app-grid.cpp`（`kAppGridCss`）及 `#winomeToastWindow` 的内联覆盖 CSS。
  - Wi-Fi 密码对话框按钮应用 `modal-dialog-linked-button`；`calendar.cpp` 为选中日期应用 `GTK_STATE_FLAG_SELECTED`，`dnd_switch` 启用 `toggle-switch` 与 `set_focus_on_click(FALSE)`。
- **全壳 `StEngine` 运行时指标与 `BoxPointer` 箭头**：
  - `shell-panel`、`quick-settings`（`.slider` 与 `.quick-settings-grid`）、`calendar`、`notifications`、`app-grid`（`.icon-grid` 与 `.page-indicator-icon`）、`ws-osd`（`.workspace-switcher` 与 `.ws-switcher-indicator`）及 `overview` / `overview-dash` / `overview-thumbs`（包含 `.window-close` 与 `.window-picker`）均已接入 `StEngine::get()`；因 `StEngine::Node` 以 `G_TYPE_OBJECT` 匹配，含 `StIcon` 等类型选择器的规则（如 `.window-close StIcon`）保留上游常量 fallback。
  - `shell-panel.cpp` 读取 `-arrow-rise`、`-arrow-base`、`-arrow-border-radius`、`-arrow-border-width`、`-boxpointer-gap` 及菜单背景/边框色，通过 `draw_popover_arrow()` 以 Cairo 绘制 `BoxPointer` 箭头。
  - `tests/test-main.cpp` 通过 `test_st_engine_theme_metrics()` 验证 `#panel`、`.quick-settings-grid`、`.slider`、`.window-close`、`.workspace-switcher` 与 `.popup-menu-content` 的主题指标解析。
