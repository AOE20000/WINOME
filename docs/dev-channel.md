# 开发者调试通道与 `winome-ctl` 指南（Developer Mode）

WINOME 内置面向自动化调试与排障的通用调试通道（`dev.*`）、时序测试接口（`test.*`）与非提权控制端 `winome-ctl.exe`。本文档对应源码 [dev-channel.cpp](../WINOME/src/dev-channel.cpp)、[shell-ipc.cpp](../WINOME/src/shell-ipc.cpp)、[ctl-protocol.h](../WINOME/ctl/ctl-protocol.h)、[winome-ctl.cpp](../WINOME/ctl/winome-ctl.cpp) 与 [main.cpp](../WINOME/src/main.cpp)。

---

## 1. 前置条件与接入入口

### 1.1 启用开发者模式

- `dev.*` 与 `test.*` 均受配置项 `developer_mode` 门控；当 `developer_mode` 为 `false` 时，调用将直接返回 `{"ok":false,"error":"developer_mode is disabled"}`。
- 开启方式：
  - UI：`Settings -> Behavior` 中开启 **Developer Mode**；
  - CLI：`winome-ctl.exe config set developer_mode true`（也可运行 `winome-ctl.exe dev op shell.config action=set key=developer_mode value=true`）。

### 1.2 三种接入方式

1. **非提权 CLI（`WINOME/dist/WINOME-portable/winome-ctl.exe` 或 `WINOME/build/ctl/winome-ctl.exe`）**
   - `winome-ctl.exe` 不含 UAC 提权清单、无 GTK4 依赖；宿主服务端通过 `ipc_build_user_pipe_sddl()` 构造 `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<USER_SID>)S:(ML;;NW;;;ME)`，允许同用户 Medium-IL 进程直连命名管道 `\\.\pipe\WINOME_Shell_IPC`。
2. **Looking Glass Command 页**
   - 开启开发者模式后通过 `Alt+F2` 输入 `lg` 或运行 `winome-ctl.exe lg` 打开 Looking Glass，在 Command 页可直接输入 `dev.help`、`dev.op ...`、`dev.batch ...`、`test.trigger ...` 等指令。
3. **原始 JSON 透传**
   - 使用 `winome-ctl.exe cmd '{"cmd":"..."}'` 直接向 IPC 服务投递 JSON 请求。

---

## 2. 连接语义、错误分类与进程识别

### 2.1 Named Pipe 错误码区分

`winome-ctl` 通过 `ctl_classify_pipe_error()`（[ctl-protocol.h](../WINOME/ctl/ctl-protocol.h)）将连接、写入与读取异常区分返回，并附带原始 Win32 错误码 `gle`，避免受限沙箱拦截与“壳体未启动”混淆：

| `error` 字段 | 触发条件（Win32 错误码 / 状态） | 典型原因与处理 |
|---|---|---|
| `not_running` | `ERROR_FILE_NOT_FOUND` (2) / `ERROR_PATH_NOT_FOUND` (3) | `winome.exe` 未启动或管道尚未创建 |
| `ipc_access_denied` | `ERROR_ACCESS_DENIED` (5) / `ERROR_PRIVILEGE_NOT_HELD` (1314) | 命令执行环境或沙箱禁止访问命名管道（并非壳体崩溃），请在沙箱外或同用户会话运行 |
| `ipc_pipe_busy_timeout` | `ERROR_PIPE_BUSY` (231) 超出连接等待时间 | 并发请求排队超过连接期限 |
| `ipc_reply_timeout` | 等待服务端响应超时（`dev` / `test` 超时 60s，常规命令 3s） | 主线程阻塞或批处理等待超时 |
| `ipc_error` | 其它 Win32 错误（如 `ERROR_BROKEN_PIPE` 109） | 对端异常断开或读取失败，结合 `gle` 排查 |

### 2.2 `status` 进程识别与 `inspect panel`

- **区分壳进程与看门狗进程**：WINOME 运行时会同时存在两个 `winome.exe` 进程——持有窗口与 IPC 管道的主壳进程，以及无窗口、负责崩溃后恢复任务栏的 `--watchdog <pid>` 守护进程。`winome-ctl.exe status` 会在结果中直接返回：
  ```json
  {"ok":true,"result":{"version":"0.3.1","pid":9576,"watchdog_pid":22880,...}}
  ```
- **面板与浮层状态快照**：`winome-ctl.exe inspect panel`（及 `winome-ctl.exe dev op shell.panel action=inspect`）经 GTK 主线程同步调用 `winome_shell_panel_inspect_json()`，返回全部已注册顶栏与当前打开 popover 的句柄及 Win32/DWM 可见性：
  - 顶栏字段：`panels[]`（`index`、`hwnd`、`visible`、`mapped`、`opacity`、`w`、`h`、`win32_visible`、`cloaked`、`topmost`、`exstyle`、`rect`、`overview_progress`）与 `panel_count`；
  - 浮层字段：`open_popovers[]`（`button`、`centered`、`hwnd`、`visible`、`mapped`、`w`、`h`、`win32_visible`、`cloaked`、`topmost`、`rect`）与 `open_popover_count`；
  - 宿主模式字段：`safe_mode`、`developer_mode`。

---

## 3. 通用调试操作通道（`dev.*`）

### 3.1 CLI 命令形态

```bash
# 获取全部受支持 op 清单（返回 JSON 中 channel="dev-universal"）
winome-ctl.exe dev help

# 执行单个 op（wait_ms 上限 15000ms；--logs / --tag 捕获操作期间产生的日志）
winome-ctl.exe dev op <op> [k=v ...] [--wait <ms>] [--logs] [--tag <T>]

# 执行串行批处理流水线（以分号 ; 分隔；自动汇报耗时与私有内存/GDI/USER 句柄增量）
winome-ctl.exe dev batch --ops "<op1 args; op2 args; ...>" [--logs] [--stop-on-error] [--tag <T>]
winome-ctl.exe dev batch --json '[{"op":"gtk.windows"},{"op":"sleep","args":{"ms":50}}]' [--logs] [--stop-on-error]
```

**简写位置参数规则（`dev_parse_op_shorthand`）**：在 `dev op` 与 `dev batch --ops` 中，若第 1 个实参不含 `=`，将按 op 自动映射默认键名：

- `sleep` → `ms`
- `key.tap` / `key.down` / `key.up` → `key`
- `key.combo` → `keys`
- `type` → `text`
- `win.focus` → `hwnd`
- `gtk.tree` / `gtk.redraw` → `window`
- `gtk.css`：若为 `list` / `clear` / `append` / `replace`（忽略大小写）则映射为 `action`，否则映射为 `css`
- 其余 op → `action`

---

### 3.2 `input`：OS 级键鼠与文本注入（10 个 op）

| op | 参数与默认值 | 说明 |
|---|---|---|
| `key.tap` | `key=<name>` `[hold_ms=25]` | 注入按下 + 延迟（5..1000ms）+ 抬起 |
| `key.down` | `key=<name>` | 注入单个虚拟键按下 |
| `key.up` | `key=<name>` | 注入单个虚拟键抬起 |
| `key.combo` | `keys=Win+Tab` `[hold_ms=30]` | 按顺序按下全部组合键，保持后逆序抬起 |
| `type` | `text=<utf8>` | 使用 `KEYEVENTF_UNICODE` 逐字符注入 UTF-8 文本 |
| `mouse.pos` | 无 | 返回当前光标物理坐标 `x`、`y` 及所在 `monitor` 句柄 |
| `mouse.move` | `x=<px> y=<px>` `[rel=false]` | 绝对移动（`rel=true` 时按当前光标相对偏移） |
| `mouse.click` | `[button=left\|right\|middle]` `[x=.. y=..]` `[double=false]` | 若同时提供 `x` 与 `y` 先移到目标坐标再单击/双击 |
| `mouse.drag` | `x1=.. y1=.. x2=.. y2=..` `[button=left]` `[steps=16]` `[duration_ms=240]` | 线性插值多步拖拽（`steps` 1..200，`duration_ms` 10..5000） |
| `mouse.wheel` | `[delta=-120]` `[horiz=false]` `[x=.. y=..]` | 垂直或水平滚轮事件 |

---

### 3.3 `window`：Win32 窗口检查与直控（6 个 op）

| op | 参数与默认值 | 说明 |
|---|---|---|
| `win.list` | 无 | 列出顶层窗口（含 Z 序、桌面归属、`topmost`、`cloaked`、`rect` 等） |
| `win.fg` | 无 | 返回当前前景窗口的 `hwnd`、`pid`、`rect`、`class`、`title` |
| `win.focus` | `hwnd=0x..` | 若窗口最小化先 `SW_RESTORE`，再调用 `SetForegroundWindow` |
| `win.move` | `hwnd=0x..` `[x=.. y=.. w=.. h=..]` | 调用 `SetWindowPos(..., SWP_NOZORDER \| SWP_NOACTIVATE)`；省略的字段沿用当前窗口值 |
| `win.state` | `hwnd=0x.. action=minimize\|maximize\|restore\|show\|hide\|close` | 异步修改窗口显示状态或投递 `WM_CLOSE` |
| `win.msg` | `hwnd=0x.. msg=<num>` `[wparam=0]` `[lparam=0]` `[post=true]` | `post=true` 走 `PostMessageW`，`post=false` 走 `SendMessageTimeoutW(..., 1000ms)` |

---

### 3.4 `gtk`：GTK4 实时内省、样式注入与可控突变（7 个 op）

所有 `gtk.*` op 均通过 `run_on_gtk_main_sync()` 在 GTK 主线程执行，并由 `resolve_gtk_toplevel()` 统一解析 `window` 选择器：
- **整数索引**：如 `window=0`、`window=3`；
- **十六进制句柄**：如 `window=0x6219A8`，直接匹配 `HWND`；
- **名称 / 标题子串**：按优先级打分，优先匹配 `visible && mapped` 的窗口，其次仅 `visible` 的窗口，避免同名隐藏辅助窗抢先命中。

| op | 参数与默认值 | 说明 |
|---|---|---|
| `gtk.windows` | 无 | 列出全部 GTK 顶层窗口：除 `index`/`visible`/`mapped`/`sensitive`/`opacity`/`w`/`h`/`scale`/`hwnd`/`renderer`/`name`/`title` 外，同步附带 Win32/DWM 字段 `win32_visible`、`cloaked`、`topmost`、`minimized`、`on_screen`、`exstyle`、`win32_rect` |
| `gtk.tree` | `[window=0\|<name>\|0x<hwnd>]` `[max_depth=8]` `[measure=false]` | 导出控件树（`max_depth` 1..20，单次节点预算 400）。包含 `opacity`/`sensitive`；窗口根节点附带 `hwnd` 与 Win32/DWM 可见性；传入 `measure=true` 时对各节点调用 `gtk_widget_measure` 输出 `min_w`/`nat_w`/`min_h`/`nat_h` |
| `gtk.css` | `[action=replace\|append\|list\|clear]` `[css=<rules>]` `[clear=true]` | 有状态内存级调试样式表（`GTK_STYLE_PROVIDER_PRIORITY_USER`）：`replace` 整表替换、`append` 增量追加、`list` 查看当前生效内容与字节数、`clear`（或旧参数 `clear=true`）移除并清空 |
| `gtk.click` | `[window=0]` `[name=.. class=.. text=..]` `[fallback=mouse]` | 对 `GtkButton` 触发 `"clicked"` 信号，其它控件尝试 `gtk_widget_activate`。失败时返回明确 `reason`（`widget_not_visible` / `widget_not_sensitive` / `widget_not_activatable_use_fallback_mouse`）；对于靠 `GtkGestureClick` 响应的自定义控件（如 `WinomeActivitiesBtn`），传入 `fallback=mouse` 将在主线程算出客户区中心物理坐标并在主线程外通过 `SendInput` 执行物理点击 |
| `gtk.prop` | `[window=0]` `[name=.. class=.. text=..]` `prop=<name>` `[value=..]` | 支持读写：`visible`、`opacity`、`sensitive`、`text`（兼容 `GtkEditable` 与 `GtkLabel`）、`classes`（写入支持 `+类名` 添加 / `-类名` 移除）；支持只读查询：`min_size`/`measure`（`min_w`/`nat_w`/`min_h`/`nat_h`/`w`/`h`）、`bounds`、`css_name`、`margins`（含 `margin_top`/`bottom`/`start`/`end` 别名） |
| `gtk.node_at` | `x=<px> y=<px>` `[window=0]` `[screen=false]` | 调用 `gtk_widget_pick()` 返回命中点最内层叶节点 `target`（含 `min/nat` 测量）及从根窗到叶节点的完整 CSS 祖先链 `chain`。默认采用窗口逻辑坐标；若直接取自屏幕截屏坐标，传 `screen=true` 自动完成 `ScreenToClient` 与缩放换算 |
| `gtk.redraw` | `[window=all\|0\|<name>\|0x<hwnd>]` | **非破坏性**表面重绘：对可见顶层窗口或指定窗口执行 `gtk_widget_queue_allocate` + `gtk_widget_queue_draw` + `InvalidateRect`，**不销毁重建 `GdkSurface`**，并返回已重绘窗口 `hwnd` 列表 |

---

### 3.5 `shell`：WINOME 子系统直调（9 个 op）

| op | 参数与默认值 | 说明 |
|---|---|---|
| `shell.overview` | `action=open\|close\|toggle\|grid\|refresh\|restack\|invalidate_wallpaper\|reposition`（默认 `toggle`） | 控制概览展开/切换应用网格/重算布局/刷新或重建壁纸表面 |
| `shell.panel` | `action=reload_theme\|close_popovers\|inspect`（默认 `inspect`） | 热重载主题、关闭所有浮层，或返回 `winome_shell_panel_inspect_json()` |
| `shell.ws` | `action=list\|next\|prev\|switch\|create\|remove`（默认 `list`） `[index=0]` | 查询或操作当前活动显示器的工作区 |
| `shell.dialog` | `target=settings\|looking_glass\|run`（默认 `looking_glass`） `action=open\|close\|toggle` `[page=..]` | 打开或关闭设置页（可指定 `page` 如 `diagnostics`/`behavior`）、Looking Glass 或运行弹窗 |
| `shell.cache` | `action=icon_clear\|icon_trim\|app_refresh\|brightness_invalidate\|systray_refresh` `[keep=64]` | 清理/裁剪应用图标纹理缓存、刷新应用索引、失效亮度缓存或强制异步扫描系统托盘 |
| `shell.config` | `action=get\|set\|reload`（默认 `get`） `[key=.. value=..]` | 读取实时配置快照、热改写配置字段或从 `%LOCALAPPDATA%\WINOME\config.json` 重载 |
| `shell.flag` | `name=zorder_trace\|hotkey_trace\|ipc_trace\|frame_overlay` `[value=true]` | 动态开关调试追踪标志，并返回完整 `debug_flags` 状态 |
| `shell.fullscreen` | `action=eval` | 立即触发一次全屏状态评估 `evaluate_fullscreen_state()` |
| `shell.win_tap` | 无 | 触发一次内部裸 `Win` 键点击模拟 `simulate_bare_win_tap()` |

---

### 3.6 `telemetry`：遥测与日志（4 个 op）

| op | 参数与默认值 | 说明 |
|---|---|---|
| `perf.snapshot` | 无 | 返回当前进程工作集/私有字节/GDI/USER 句柄计数、启动阶段耗时与概览帧率统计 |
| `log.tail` | `[count=40]` `[tag=..]` | 读取环形缓冲区最近日志（`count` 1..512，可按 `tag` 过滤） |
| `log.clear` | 无 | 清空内存调试日志环形缓冲区 |
| `sleep` | `[ms=100]` | 同步等待指定毫秒（0..30000ms），常用于 `dev batch` 步骤间稳定等待 |

---

## 4. 自动化测试接口（`test.*`）

适用于触发并采集 `[perf]` 帧率、内存前后增量与分段日志序列：

| 子命令 | 用法与说明 |
|---|---|
| `test trigger` | `winome-ctl.exe test trigger <action> [--wait ms] [--tag T]`（默认 `wait_ms=450`）。内置动作：`win_tap_os`、`win_tap`、`esc_os`、`overview_open`、`overview_close`、`desktop_next`、`desktop_prev`、`desktop_switch:<index>` |
| `test run` | `winome-ctl.exe test run (--steps CSV \| --preset P) [--gap ms] [--settle ms] [--repeat N] [--tag T]`。内置预设 P 包括：`slow-double-win`（默认间隔 380ms）、`slow-double-win-internal`（380ms）、`fast-double-win`（120ms）、`overview-cycle`（500ms）、`overview-bench`（450ms，打开并切换至应用网格后 `esc_os` 收起） |
| `test begin` / `test end` | 跨多条外部命令开启与结束聚合追踪会话，在 `test end` 汇总整段增量遥测与日志 |

---

## 5. 实操排查范式与注意事项

1. **三层交叉定位视觉故障**
   - **Win32 层定位置与遮罩**：先用 `winome-ctl.exe inspect panel`、`winome-ctl.exe dev op win.list` 与 `gtk.windows` 确认 `win32_rect`、`topmost`、`cloaked` 与 `on_screen`（GTK 侧 `visible=true` 不代表窗口未被 DWM 遮罩或未被其它置顶窗挡住）。
   - **GTK 层查布局与命中**：通过 `gtk.tree window=0x... max_depth=8 measure=true` 对比 `min_w`/`nat_w`/`min_h`/`nat_h` 与实际分配 `w`/`h`；若需确定屏幕上某坐标像素归属哪个节点，直接执行 `gtk.node_at x=<px> y=<px> screen=true`，无需反复染色试错。
   - **安全热验证样式与重绘**：先用 `gtk.css action=append css="..."` 叠加规则测试（可随时 `gtk.css list` 审查累积规则），验证完成后务必执行 `gtk.css clear` 还原；遇到显示滞留时优先尝试非破坏性的 `gtk.redraw`，不要在生产或非调试场景强行破坏窗口表面。
2. **自定义控件点击**：顶栏 `WinomeActivitiesBtn` 等采用 `GtkGestureClick` 的组件不会响应 `gtk_widget_activate`，调用 `gtk.click` 时请显式传入 `fallback=mouse`。
3. **沙箱环境注意**：若 `winome-ctl` 返回 `{"ok":false,"error":"ipc_access_denied","gle":5}`，说明命令运行环境拦截了 Named Pipe 访问，需移出沙箱或确保与壳进程处于同一用户会话后重测。
