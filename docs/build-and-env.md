# 开发环境与构建指南 (Windows / MSYS2 UCRT64)

MSYS2 已安装到 `E:\msys64`（UCRT64 工具链），GTK4 开发环境就绪。

## 已安装工具

| 工具 | 版本 | 路径 |
|------|------|------|
| GTK4 | 4.22.4 | `E:\msys64\ucrt64` |
| GCC | 16.2.0 | `E:\msys64\ucrt64\bin\gcc.exe` |
| Meson | 1.12.0 | `E:\msys64\ucrt64\bin\meson.exe` |
| Ninja | 1.13.2 | `E:\msys64\ucrt64\bin\ninja.exe` |
| pkg-config | 3.0.5 | `E:\msys64\ucrt64\bin\pkg-config.exe` |
| sassc | 3.6.2 | `E:\msys64\ucrt64\bin\sassc.exe` |

### 环境变量（用户级）

- `MSYS2_ROOT=E:\msys64`
- `MSYSTEM=UCRT64`
- `XDG_DATA_DIRS=E:\msys64\ucrt64\share`

### 依赖包安装参考

```bash
pacman -S mingw-w64-ucrt-x86_64-gtk4 mingw-w64-ucrt-x86_64-pkgconf \
          mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-gcc
```

### 调用方式

推荐在 MSYS2 UCRT64 终端内开发（`E:\msys64\msys2.exe` 或 `msys2_shell.cmd -ucrt64`）。

在 PowerShell 中调用时，`gcc` 直调可能无法定位 `cc1`，请使用 bash 包装：

```powershell
E:\msys64\usr\bin\bash.exe -lc 'export PATH=/ucrt64/bin:$PATH && ...'
```

---

## 构建方案（`build.sh`）

WINOME 主工程在 `WINOME/` 目录下，统一使用仓库根目录的交互式构建脚本 [build.sh](../build.sh)：

```bash
./build.sh            # 交互式菜单（显示各方案产物状态）
./build.sh release    # 生产：release + LTO + NDEBUG -> WINOME/build-release/
./build.sh test       # 测试：debugoptimized         -> WINOME/build/
./build.sh debug      # 调试：-O0 -g                 -> WINOME/build-debug/
./build.sh run        # 运行最近构建的 winome.exe
./build.sh clean      # 清理全部构建目录
```

| 方案 | 配置 | 构建目录 | 实测产物基线 | 说明 |
|------|------|----------|--------------|------|
| `release` | `release` + LTO + `NDEBUG` | `WINOME/build-release/` | ~1.3MB–1.4MB | 最小体积、最快指令（内存基线 ~30MB） |
| `test` | `debugoptimized` | `WINOME/build/` | ~4.6MB–4.9MB | 项目默认，保留 `g_print` 诊断日志与断言 |
| `debug` | `debug` (`-O0 -g`) | `WINOME/build-debug/` | — | GDB 友好 |

三方案使用**独立构建目录**，切换互不触发重编；LTO 安全（COM 接口走运行时 `vcall` 裸函数指针，链接器无法去虚拟化）。

### 手动调用 Meson / Ninja

手动构建（等价于 `test` 方案，且需显式带上 ASCII 临时目录环境变量）：

```bash
export PATH=/ucrt64/bin:$PATH
export TMP='E:\tmp_winome' TEMP='E:\tmp_winome' TMPDIR='E:\tmp_winome'
cd WINOME
meson setup build
ninja -C build
./build/src/winome.exe   # 需管理员权限运行
```

> Meson 1.12 下若执行 `meson setup --reconfigure`，必须先 `cd WINOME` 并传相对 builddir（如 `build`）；传绝对路径可能将 builddir 误判为 sourcedir 而失败。

---

## 已知构建问题与对策

### 1. `gcc-ar` 覆盖静态库静默失败

- **现象**：`ninja` 报 `FAILED: [code=1] st-compat/libstcompat.a`，没有任何 stderr 输出，构建立即中止。
- **原因**：在中文用户名路径环境下，`gcc-ar` 覆盖已存在的 `libstcompat.a` 时静默返回 1（逐个 `.obj` 归档均成功，仅覆盖目标 `.a` 时失败；正常归档约 1.4MB，损坏时常见仅十余 KB）。
- **对策**：`build.sh` 每次执行 `ninja` 前通过 `clean_stale_static_lib()` 自动删除 `st-compat/libstcompat.a`；手动执行 `ninja` 遇到同样报错时先执行 `rm build*/st-compat/libstcompat.a` 再重编。

### 2. LTO 链接临时路径失败（仅 `release` 方案）

- **现象**：
  ```text
  lto1.exe: fatal error: could not open symbol resolution file: No such file or directory
  lto-wrapper.exe: fatal error: cc returned 1 exit status
  ```
- **原因**：LTO 链接时 GCC 将临时目录原样传给原生 `collect2.exe` / `lto1.exe`，后者按 **Win32 路径**解析。MSYS 风格的 `/tmp` 在中文用户名下无法解析。该问题仅影响 `b_lto=true` 的 `release` 方案，`test`（`debugoptimized`）不走 LTO。
- **对策**：`TMP` / `TEMP` / `TMPDIR` 必须设为 **Windows 绝对 ASCII 路径**，`build.sh` 默认设为 `E:\tmp_winome`，可用环境变量覆盖：
  ```bash
  WINOME_TMP='D:\some\ascii\path' ./build.sh release
  ```

### 3. 链接失败伪像：运行中进程占用 `winome.exe`

- **现象**：`collect2.exe: error: ld returned 1 exit status`，且没有任何 undefined symbol。
- **原因**：正在运行的 `winome.exe` 实例锁住了输出文件。
- **对策**：重新链接前先退出正在运行的 `winome.exe`。

### 4. GCC 全程序 LTO `-Wstringop-overread` 误报

- **说明**：`WINOME/src/overview-dash.cpp` 的 `window_exe_path()` 曾因 `return L""` 在完整 LTO 内联链下触发 `-Wstringop-overread`（单文件编译不复现），已改为 `return {}` 消除。
