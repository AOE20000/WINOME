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

WINOME 主工程在 `WINOME/` 目录下：

```bash
cd WINOME
meson setup build
ninja -C build
./build/src/winome.exe   # 需管理员权限运行
```

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
