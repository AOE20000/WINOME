# GTK 开发环境 (Windows)

MSYS2 已安装到 `E:\msys64`（UCRT64 工具链），GTK4 开发环境就绪。

## 已安装工具

| 工具        | 版本      | 路径                          |
| ----------- | --------- | ----------------------------- |
| GTK4        | 4.22.4    | `E:\msys64\ucrt64`            |
| GCC         | 16.2.0    | `E:\msys64\ucrt64\bin\gcc.exe`|
| Meson       | 1.12.0    | `E:\msys64\ucrt64\bin\meson.exe` |
| Ninja       | 1.13.2    | `E:\msys64\ucrt64\bin\ninja.exe` |
| pkg-config  | 3.0.5     | `E:\msys64\ucrt64\bin\pkg-config.exe` |
| sassc       | 3.6.2     | `E:\msys64\ucrt64\bin\sassc.exe` |

## 使用方式

推荐在 MSYS2 UCRT64 终端内开发（`E:\msys64\msys2.exe` 或 `msys2_shell.cmd -ucrt64`）。

PowerShell 中也可以直接调用（`E:\msys64\ucrt64\bin` 已加入用户 PATH），但
`gcc` 从 PowerShell 直调时可能无法定位 `cc1`，请使用 bash 包装：
`E:\msys64\usr\bin\bash.exe -lc 'export PATH=/ucrt64/bin:$PATH && ...'`

## Meson 构建示例

WINOME 主工程在 `WINOME/` 目录下，推荐用根目录的交互式构建程序：

```bash
./build.sh            # 交互式菜单（生产/测试/调试三方案）
./build.sh release    # 生产：release + LTO + NDEBUG -> WINOME/build-release/
./build.sh test       # 测试：debugoptimized         -> WINOME/build/
./build.sh debug      # 调试：-O0 -g                  -> WINOME/build-debug/
```

也可手动调用 meson（等价于测试方案）：

```bash
cd WINOME
meson setup build
ninja -C build
./build/src/winome.exe   # 需管理员权限运行
```

### 已知问题：gcc-ar 静默归档失败

现象：`ninja` 报 `FAILED: [code=1] st-compat/libstcompat.a`，**没有任何错误输出**，
且失败点在链接第一步（归档），后续步骤全部连带中止。

原因：本机（中文用户名路径）`gcc-ar` 覆盖已存在的 `libstcompat.a` 时静默失败。
与源码无关——逐个 `.obj` 归档都成功、27 个 obj 齐全，只有覆盖写目标 `.a` 时挂。

对策：`build.sh` 已在每次 ninja 之前自动删除该归档（`clean_stale_static_lib`）。
**若手动调 ninja 遇到同样报错，先 `rm build/st-compat/libstcompat.a` 再重编**，
不要误判为源码错误。正常归档约 1.4MB，若你的 `.a` 只有十几 KB 基本可以确定已损坏。

### 已知问题：LTO 链接失败（仅 release 方案）

现象：

```
lto1.exe: fatal error: could not open symbol resolution file: No such file or directory
lto-wrapper.exe: fatal error: cc returned 1 exit status
```

原因：LTO 链接时 gcc 把临时目录原样传给原生 `collect2.exe` / `lto1.exe`，
后者按 **Win32 路径**解析。`build.sh` 早期设的 `TMP=/tmp` 是 MSYS 风格路径，
原生程序无法解析，在中文用户名下直接失败。只有 `b_lto=true` 的 release 方案受影响，
`test`（debugoptimized）不走 LTO 所以一直正常——这也是该问题长期没暴露的原因。

对策：`TMP/TEMP/TMPDIR` 必须设为 **Windows 绝对 ASCII 路径**，现默认为 `E:\tmp_winome`。
可用环境变量覆盖：

```bash
WINOME_TMP='D:\some\ascii\path' ./build.sh release
```

手动调 ninja 时同样要带上，否则 LTO 链接必挂。

## 环境变量（已写入用户级）

- `MSYS2_ROOT=E:\msys64`
- `MSYSTEM=UCRT64`
- `XDG_DATA_DIRS=E:\msys64\ucrt64\share`

## 安装的包

```
pacman -S mingw-w64-ucrt-x86_64-gtk4 mingw-w64-ucrt-x86_64-pkgconf \
          mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-gcc
```
