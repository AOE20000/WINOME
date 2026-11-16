# Windows 独立文件管理器技术调研与参考仓库索引

> 本文归档「不依赖微软高层方案 / 避开导致卡顿的微软方案 / 借鉴 Go 家庭存储项目架构」的完整技术调研结论，并索引已拉取至 `upstream/` 的 8 个参考仓库。本文件为独立新增调研文档，未改动仓库现有代码与文档。

---

## 1. 问题边界：什么叫“不依赖微软方案”

在 Windows 上脱离微软生态开发文件管理器，必须先区分**可彻底剥离的上层框架与 Shell 生态**和**不可逾越的操作系统内核边界**：

1. **彻底剥离上层框架与 Explorer/Shell COM（完全可行，工程主目标）**
   - **放弃的应用框架与工具链**：WinUI 3 / Windows App SDK、UWP、WPF / WinForms / MAUI、WebView2、MSVC / C++/WinRT；
   - **放弃的 Explorer / Shell COM 方案**：`IExplorerBrowser`、`IShellFolder` / `PIDL` / `IShellItem`、`IPropertyStore`、`IShellIconOverlayIdentifier`、`IFileOperation` / `SHFileOperationW`、`IContextMenu` / `IExplorerCommand`、`IThumbnailProvider` / `IShellItemImageFactory`、`SHGetFileInfo`、`Windows.Storage`、`WSearch`（Windows Search）。
   - **保留最小 OS 内核边界**：Windows 上所有用户态进程最终都必须经过 `ntdll.dll` / `kernel32.dll` 进入内核（`ntoskrnl.exe` / `ntfs.sys`）。
2. **警惕 GLib / GIO 在 Windows 下的隐藏 Shell 依赖（实测陷阱）**
   - 即使使用跨平台的 GTK4 + GLib/GIO，GLib 的默认 Win32 后端仍部分包装了微软 Shell 与注册表：
     - `g_file_trash()` 在 Win32 下调用 `SHFileOperationW(..., FO_DELETE, FOF_ALLOWUNDO)`；
     - `g_content_type_*` 编译的是 `gcontenttype-win32.c`（查询 `HKEY_CLASSES_ROOT`），而非使用 Linux 常用的 `xdgmime` / `shared-mime-info`；
     - `g_app_info_*`（`gwin32appinfo.c`）读取 `HKEY_CLASSES_ROOT\...\shell\<verb>\command`；
     - GTK 4.10+ 的 `GtkFileDialog` 内部封装 `IFileDialog` COM；
     - `GFileMonitor` 在 Win32 下走 `gwin32filemonitor.c` 封装 `ReadDirectoryChangesW`（属内核 API，非 Shell）。
   - 因此，若追求彻底不走微软 Shell/注册表，需绕过 GIO 上述 Win32 适配层。
3. **绕开内核文件系统驱动直读写磁盘的硬边界**
   - **只读索引可行（需管理员权限）**：通过卷句柄 `\\.\C:` 配合 `FSCTL_ENUM_USN_DATA` / `FSCTL_READ_USN_JOURNAL` 或只读解析 `$MFT`，可跳过逐目录递归；但标准非提权用户调用会被内核拒绝（`ERROR_ACCESS_DENIED`）。
   - **直接扇区写入不可行**：Windows 自 Vista 起禁止在未锁定卷的情况下对已挂载卷写原始扇区，且页缓存与日志由 `ntfs.sys` 独占维护。若需向操作系统全局挂载自定义虚拟文件系统，应使用开源内核桥 **WinFsp**。

---

## 2. 两条务实工程路线对照

| 维度 | 路线 A：彻底不依赖微软高层方案 | 路线 B：只避开导致卡顿的微软方案（推荐务实路线） |
|---|---|---|
| **目标定位** | 零 Shell COM、零注册表关联、自研二进制解析与独立生态 | **外科手术式去 Shell**（Total Commander / Double Commander 思路）：砍掉致卡机制，保留快且无害的系统接口 |
| **UI 层** | **GTK4 + libadwaita**（虚拟化 `GtkColumnView` / `GtkGridView`） | 同左（彻底摆脱 Win11 `explorer.exe` 的 XAML Islands / WinUI 3 跨 `HWND` 同步卡顿） |
| **目录枚举** | `ntdll!NtQueryDirectoryFile`（`FileIdBothDirectoryInformation`）或 `FindFirstFileExW(FindExInfoBasic)` | 同左（避开 `IShellFolder` 加载第三方扩展、AFTD 自动文件夹类型嗅探与图标覆盖扩展） |
| **文件监控** | 异步 `OVERLAPPED` / IOCP 驱动 `ReadDirectoryChangesW` | 同左（必须脱离 UI/STA 线程，并做防抖与 Junction 独立监控） |
| **复制 / 移动 / 删** | 自研线程池 `CreateFileW` / `CopyFile2` + `SetFileInformationByHandle`；自写 `$Recycle.Bin` 的 `$I`/`$R` 二进制对 | 大批量文件走目录骨架预创建 + 8–16 线程并发 `CopyFile2`；**仅回收站移入与 UAC 提权写入按需异步单批调 `IFileOperation`** |
| **MIME 与图标** | `xdgmime` 直读 `shared-mime-info`（`mime.cache`）+ Adwaita SVG + 自写 PE `.rsrc` 图标提取与 `[MS-SHLLINK]` `.lnk` 解析 | **`SHGetFileInfoW(SHGFI_SYSICONINDEX \| SHGFI_USEFILEATTRIBUTES)` 按后缀内存缓存**（不碰磁盘/扩展），特殊项后台异步补图 |
| **缩略图** | 自建核心数限流线程池：`gdk-pixbuf` / `librsvg` / `libheif` / `libavif` / `libraw` / `poppler` / `ffmpeg` + XDG PNG 缓存 | 同左，或本地图片用共享 `IStream` 喂给 **WIC `IWICImagingFactory`** 先取内嵌 EXIF 缩略图并立刻释放流（**避开 `IThumbnailProvider` 跨进程与锁文件**） |
| **右键菜单** | 100% 自绘右键菜单 + 独立脚本/插件体系（如 `libpeas2` 或子进程 IPC） | 主右键菜单 100% 自绘（瞬时弹出）；“系统第三方扩展菜单”折叠到二级菜单或按 `Shift` 后置在独立子进程按需懒加载 `IContextMenu` |
| **文件打开与热键** | 自管关联表 / 只读 `HKCR` 后 `CreateProcessW`；`WH_KEYBOARD_LL` 接管 `Win+E` | 双击打开那一刻在后台线程调用 **`ShellExecuteExW`**（零列表卡顿，继承 `UserChoice` / AppX / UAC）；`WH_KEYBOARD_LL` 接管 `Win+E` |
| **远程与便携设备** | `libarchive` + `libssh2` + `libcurl` + `libsmb2`（MTP 用 `libmtp`+WinUSB 兼容成本极高，慎选） | 压缩包/SFTP/WebDAV 用开源库；SMB3 保留内核 UNC（`\\server\share`）+ 可取消异步工作线程；WPD 仅点入手机节点时懒加载 |

---

## 3. Windows 原生实现的核心避坑要点

### 3.1 必须识别的四类 NTFS 重解析点（Reparse Points）
不经过 `IShellFolder` 后，目录项出现 `FILE_ATTRIBUTE_REPARSE_POINT` 时必须查看 `dwReserved0` 或通过 `DeviceIoControl(FSCTL_GET_REPARSE_POINT)` 识别：
- `IO_REPARSE_TAG_MOUNT_POINT`（目录联接 Junction）与 `IO_REPARSE_TAG_SYMLINK`：计算目录大小、递归搜索时必须判断 `IsReparseTagNameSurrogate()` 防止死循环；
- `IO_REPARSE_TAG_CLOUD`（OneDrive 等云占位符）：**严禁在列目录、悬停或生成缩略图时直接打开文件流读取**，否则内核回调会强制触发云端下载（Hydration）。应先用 `GetFileInformationByHandleEx(FileAttributeTagInfo)` 检查离线/占位状态；
- `IO_REPARSE_TAG_APPEXECLINK`（`%LOCALAPPDATA%\Microsoft\WindowsApps` 下的 0 字节应用执行别名）：需通过 `FSCTL_GET_REPARSE_POINT` 解析真实目标。

### 3.2 兼容系统回收站而不调 Shell API（`$Recycle.Bin` 二进制规范）
Windows Vista 及以上在每个卷 `X:\$Recycle.Bin\<用户SID>\` 下存放成对文件：
- `` `$R<6位随机字符>.<原扩展名>` ``：原封不动移动过来的文件数据或目录树；
- `` `$I<同6位字符>.<原扩展名>` ``：小端序元数据文件：
  - **前 24 字节公共头**：`UINT64 version`（Win10/11 为 `2`，Vista–Win8.1 为 `1`） + `UINT64 original_size` + `FILETIME deleted_utc`；
  - **v2 后续（Win10/11）**：`UINT32 path_char_len` + `UTF-16LE` 绝对原路径（含结尾 `0x0000`）。

### 3.3 不依赖 `IShellLinkW` / `SHGetFileInfo` 的二进制解析路径
- **`.lnk` 解析**：按微软公开 `[MS-SHLLINK]` 二进制格式顺序解析（76 字节 `ShellLinkHeader` → 可选 `LinkTargetIDList` → `LinkInfo` 中的 `LocalBasePath` → `StringData` 的 `Name` / `RelativePath` / `WorkingDir` / `Arguments` / `IconLocation`），无 COM 套间阻塞风险。
- **PE 图标提取**：解析 `.exe`/`.dll` 的 DOS/NT 头定位 `.rsrc` 节区，遍历 3 级资源树定位 `RT_GROUP_ICON` (`14`) 与 `RT_ICON` (`3`)，在内存中按 ICO 头（6 字节 `ICONDIR` + `ICONDIRENTRY` 数组 + 原始数据块）重拼为标准 `.ico` 字节流直接喂给 `GdkTexture`。

### 3.4 接管 `Win+E` 的安全边界
切忌无保护地直接把注册表 `HKEY_CLASSES_ROOT\Folder\shell\open\command` 改写为第三方程序——`explorer.exe` 自身内部导航也走该 `open` 动词，缺失 `DelegateExecute` COM 路由会破坏系统资源管理器内部双击跳转并引发递归死循环。在 WINOME 体系中，可直接复用 [WINOME/src/overview-trigger.cpp](../WINOME/src/overview-trigger.cpp) 已挂载的独立线程 `WH_KEYBOARD_LL` 键盘钩子拦截 `Win+E` 唤醒文件管理器。

---

## 4. 本机 MSYS2 UCRT64 非微软生态依赖现状

基于本机 `E:\msys64\ucrt64` 实查结果（与 [WINOME/package.sh](../WINOME/package.sh) 打包链路一致）：

- **已安装**：`gtk4` (`4.22.4-1`)、`glib2` (`2.88.3-1`)、`adwaita-icon-theme` (`50.0-1`)、`shared-mime-info` (`2.5.1-1`，数据位于 `/ucrt64/share/mime/`，含 `mime.cache`/`globs2`/`magic`/`subclasses`/`icons`)、`librsvg` (`2.62.3-1`)、`libwebp` (`1.6.0-1`)、`gstreamer` (`1.28.6-1`)、`sqlite3` (`3.53.4-1`)、`libcurl` (`8.21.0-1`)、`libssh2` (`1.11.1-1`)
- **UCRT64 源内可直接安装**：`libadwaita` (`1.9.3-1`)、`libarchive` (`3.8.9-2`)、`poppler` (`26.07.0-1`)、`ffmpeg` (`9.0.1-1`)、`libheif` (`1.23.1-3`)、`libavif` (`1.4.2-1`)、`libraw` (`0.22.2-1`)、`libssh` (`0.12.2-1`)、`libpeas2` (`2.2.1-1`)、`file` (`5.48-1`)、`7zip` (`26.02-1`)
- **UCRT64 源内未预置（需源码引入或外置）**：`libsmb2`、`libmtp`、`WinFsp`、`glycin`、`tinysparql`

---

## 5. 参考仓库索引与源码导读（`upstream/`）

已浅克隆（`--depth 1`）以下 8 个参考项目至 `upstream/`（按 `.gitignore` 规则不随主仓库发布，仅供本地研读）：

| 仓库目录 | 上游项目 | 许可证（以仓库原文为准） | 核心借鉴点与关键源码路径 |
|---|---|---|---|
| `upstream/nautilus/` | GNOME Files (Nautilus) | `GPL-3.0-or-later` | **GTK4 + libadwaita 文件管理器 UI 真源**（仅参考 UI 结构与交互，勿直接照搬 Linux GVfs/D-Bus）：`src/nautilus-grid-view.c`（`GtkGridView`）、`src/nautilus-list-view.c`（`GtkColumnView`）、`src/nautilus-view-model.h`（`NautilusViewModel`）、`src/resources/style.css`（`.nautilus-pathbar`、`.nautilus-progress-indicator`） |
| `upstream/rclone/` | Rclone | `MIT` | **跨平台 VFS 缓存、并发分片流与 Windows 底层避坑教科书**：`fs/chunkedreader/parallel.go`（多线程分片预读）、`lib/file/unc_windows.go`（`UNCPath()` 自动转换 `\\?\` 与 `\\?\UNC\` 长路径）、`lib/file/file_windows.go`（`OpenFile()` 强制追加 `FILE_SHARE_DELETE` 与保留设备名检查）、`lib/file/preallocate_windows.go`（直调 `ntdll.dll` 的 `NtQueryVolumeInformationFile` / `NtSetInformationFile` 预分配簇）、`backend/local/remove_windows.go`（`ERROR_SHARING_VIOLATION` 指数退避重试） |
| `upstream/OpenList/` | OpenList（AList 社区分支） | `AGPL-3.0` | **家庭存储聚合 VFS 架构**（仅架构借鉴，禁止直接并入 `GPL-2.0-only` 二进制）：`internal/driver/driver.go`（最小 `Driver` 接口 + `Getter`/`Mkdir`/`Move`/`Rename`/`Copy`/`Remove`/`Put` 可选能力接口 + 全链路 `context.Context` 取消）、`internal/fs/fs.go`（虚拟挂载路径路由） |
| `upstream/filebrowser-quantum/` | FileBrowser Quantum | `Apache-2.0` | **消灭大目录与搜索卡顿的索引/预览流水线**：`backend/pkg/indexing/indexingScanner.go` 与 `windows.go`（`CheckWindowsHidden` 与按簇大小计算）、`backend/pkg/indexing/search.go`（`sessionInProgress` 抢占式取消旧搜索查询）、`backend/internal/preview/memory_tracker.go`（信号量 + 内存上限双控缩略图调度器 `MemoryTracker`） |
| `upstream/xdgmime/` | freedesktop.org xdgmime | `LGPL-2.1-or-later or AFL-2.0` | **零注册表 MIME 识别模块**：`meson.build` + `src/xdgmime.h` / `src/xdgmimecache.c` / `src/xdgmimeglob.c` / `src/xdgmimemagic.c`，可直接内存映射解析 `shared-mime-info` 的 `mime.cache` |
| `upstream/libsmb2/` | libsmb2 | `LGPL-2.1-or-later`（`libdcerpc/` 与 `examples/` 为 `BSD-2-Clause`） | **纯 C 非微软用户态 SMB2/3 客户端**：`lib/smb2-cmd-query-directory.c`、`lib/smb2-cmd-notify-change.c`、`lib/smb2-data-reparse-point.c`、`lib/smb3-seal.c` |
| `upstream/go-smb2/` | go-smb2 | `BSD-2-Clause` | **纯 Go 用户态 SMB2/3 客户端**：`client/` 与 `notify/notify.go`（展示远程目录变更监听 `Filter`/`Event`/`RescanRequired` 状态模型） |
| `upstream/lf/` | lf (List Files) | `MIT` | **极简跨平台 Go 文件管理器**：`os_windows.go`、`df_windows.go`、`watch.go`（`fsnotify` 防抖与符号链接别名映射 `getSameDirs`；注意其 `isHidden()` 逐文件调用 `GetFileAttributes` 属于反面教材，应改为复用枚举阶段的 `dwFileAttributes`） |

---

## 6. 开源许可证合规提醒（针对 WINOME）

[WINOME 工程](../WINOME/meson.build) 当前声明为 **`GPL-2.0-only`**：
- **不可直接合入或静态链接到 `winome.exe`（`GPL-2.0-only`）**：
  - `upstream/nautilus`（`GPL-3.0-or-later`）
  - `upstream/OpenList`（`AGPL-3.0`）
  - `upstream/filebrowser-quantum`（`Apache-2.0` 与 `GPL-2.0-only` 不兼容）
- **可兼容合入 / 链接**：
  - `MIT`（`rclone`、`lf`）
  - `BSD-2-Clause`（`go-smb2`）
  - `LGPL-2.1-or-later`（`xdgmime`、`libsmb2` 的 `lib/` 部分）
- **建议策略**：如果未来在 WINOME 旁开发独立文件管理器进程（例如独立可执行文件并通过 Named Pipe / CLI 与 WINOME 通信），可选用 `GPL-3.0-or-later` 释放更多上游兼容空间；若并入现有 `winome.exe`，上述 `GPL-3.0` / `AGPL-3.0` / `Apache-2.0` 仓库仅作黑盒架构与样式规律参考，所有实现须独立编写并标明 `GPL-2.0-only`。
