#!/bin/bash
# =============================================================================
# WINOME 交互式自动构建程序 (MSYS2 UCRT64 + meson/ninja)
#
# 用法:
#   ./build.sh            进入交互式菜单
#   ./build.sh release    直接执行「生产」方案
#   ./build.sh test       直接执行「测试」方案
#   ./build.sh debug      直接执行「调试」方案
#   ./build.sh clean      清理全部构建目录
#   ./build.sh run        运行最近构建的 winome.exe
#
# 构建方案 (各自独立构建目录，切换方案互不影响):
#   生产  release + LTO + NDEBUG  -> build-release/  最小体积、最快指令
#   测试  debugoptimized         -> build/          项目默认，保留 g_print
#                                                    诊断日志与断言
#   调试  debug (-O0 -g)          -> build-debug/   GDB 友好
# =============================================================================

set -euo pipefail

# MSYS2 UCRT64 工具链；中文用户名下 gcc 的临时目录必须重定向到 ASCII 路径，
# 否则汇编阶段报 "can't create ... .o" (见项目备忘)。
#
# 注意 TMP 必须是 **Windows 绝对路径**（E:\...），不能是 MSYS 风格的 /tmp：
# LTO 链接时 gcc 把临时目录原样传给原生 collect2.exe/lto1.exe，
# 后者按 Win32 路径解析，收到 "/tmp" 会在中文用户名下报
#   lto1.exe: fatal error: could not open symbol resolution file
# 该故障只在 b_lto=true 的 release 方案出现，debugoptimized 不走 LTO 故不受影响。
export PATH=/ucrt64/bin:$PATH
WINOME_TMP="${WINOME_TMP:-E:\\tmp_winome}"
mkdir -p "$WINOME_TMP" 2>/dev/null || true
export TMP="$WINOME_TMP" TEMP="$WINOME_TMP" TMPDIR="$WINOME_TMP"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$ROOT/WINOME"

# --- 颜色输出 -----------------------------------------------------------------
if [ -t 1 ]; then
  C_RED=$'\e[31m'; C_GREEN=$'\e[32m'; C_YELLOW=$'\e[33m'
  C_CYAN=$'\e[36m'; C_BOLD=$'\e[1m'; C_DIM=$'\e[2m'; C_RESET=$'\e[0m'
else
  C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''; C_BOLD=''; C_DIM=''; C_RESET=''
fi

info() { printf '%s\n' "${C_CYAN}==>${C_RESET} $*"; }
ok()   { printf '%s\n' "${C_GREEN} ✓ ${C_RESET} $*"; }
warn() { printf '%s\n' "${C_YELLOW} ! ${C_RESET} $*"; }
fail() { printf '%s\n' "${C_RED} ✗ ${C_RESET} $*" >&2; }

# --- 方案定义 -----------------------------------------------------------------
# 设置全局变量: NAME(显示名) DIR(构建目录) MESON_OPTS(meson setup 选项)
scheme_info() {
  case "$1" in
    release)
      NAME="生产 (release + LTO + NDEBUG)"
      DIR="build-release"
      MESON_OPTS=(--buildtype=release -Db_ndebug=true -Db_lto=true)
      ;;
    test)
      NAME="测试 (debugoptimized，项目默认)"
      DIR="build"
      MESON_OPTS=(--buildtype=debugoptimized)
      ;;
    debug)
      NAME="调试 (debug，-O0 -g)"
      DIR="build-debug"
      MESON_OPTS=(--buildtype=debug)
      ;;
    *)
      return 1
      ;;
  esac
}

# 方案产物状态: 构建时间 + 大小（或"未构建"）
scheme_status() {
  local key=$1 exe
  scheme_info "$key"
  exe="$PROJ/$DIR/src/winome.exe"
  if [ -f "$exe" ]; then
    printf '%s  %s' "$(date -r "$exe" '+%m-%d %H:%M')" "$(du -h "$exe" | cut -f1)"
  else
    printf '%s' "${C_DIM}未构建${C_RESET}"
  fi
}

# --- 核心动作 -----------------------------------------------------------------

# gcc-ar 在本机（中文用户名路径）覆盖已存在的 libstcompat.a 时会静默失败：
# 退出码 1 且没有任何 stderr，ninja 只报 "FAILED: [code=1] st-compat/libstcompat.a"。
# 症状与源码无关——27 个 .obj 齐全、逐个归档都成功，只有覆盖写目标 .a 时挂。
# 已在 build/ 与 build-release/ 各复现一次（均为 8/27 的残缺产物，约 14.6KB，
# 正常应为 1.4MB 量级）。
#
# 归档是可再生的中间产物，删掉最省事；在 ninja 之前无条件清掉，避免每次切换
# 构建方案都要手工 rm。
clean_stale_static_lib() {
  local dir=$1 lib="$1/st-compat/libstcompat.a"
  if [ -f "$lib" ]; then
    rm -f "$lib"
    info "已清理陈旧的 st-compat/libstcompat.a（规避 gcc-ar 静默归档失败）"
  fi
}

do_build() {
  local key=$1
  scheme_info "$key" || { fail "未知方案: $key"; exit 1; }
  local dir="$PROJ/$DIR" t0=$SECONDS

  info "方案: $NAME"
  info "目录: $DIR/"

  if [ ! -d "$ROOT" ] || [ ! -f "$PROJ/meson.build" ]; then
    fail "找不到 $PROJ/meson.build"
    exit 1
  fi

  if [ ! -f "$dir/build.ninja" ]; then
    info "首次配置..."
    (cd "$PROJ" && meson setup "${MESON_OPTS[@]}" "$DIR")
  else
    # reconfigure 保证方案选项与目录一致；配置没变时 ninja 增量构建不受影响
    # (必须 cd 到源码目录用相对 builddir：绝对路径形式在 meson 1.12 下
    #  会把 builddir 误判为 sourcedir 而失败)
    (cd "$PROJ" && meson setup --reconfigure "${MESON_OPTS[@]}" "$DIR")
  fi

  clean_stale_static_lib "$dir"

  ninja -C "$dir"

  local exe="$dir/src/winome.exe"
  if [ -f "$exe" ]; then
    ok "构建成功: $DIR/src/winome.exe ($(du -h "$exe" | cut -f1))，用时 $((SECONDS - t0))s"
  else
    fail "构建完成但未找到产物"
    exit 1
  fi
}

do_run() {
  local best="" best_mtime=0 d exe m
  for d in build build-release build-debug; do
    exe="$PROJ/$d/src/winome.exe"
    if [ -f "$exe" ]; then
      m=$(stat -c %Y "$exe" 2>/dev/null || stat -f %m "$exe")
      if [ "$m" -gt "$best_mtime" ]; then best_mtime=$m; best="$exe"; fi
    fi
  done
  if [ -z "$best" ]; then
    fail "没有找到任何构建产物，请先构建"
    return 1
  fi
  info "运行: $best ($(date -r "$best" '+%m-%d %H:%M'))"
  warn "提示: 虚拟桌面等 COM 功能需要管理员权限终端运行"
  "$best"
}

do_clean() {
  local d
  for d in build build-release build-debug; do
    if [ -d "$PROJ/$d" ]; then
      rm -rf "$PROJ/$d"
      ok "已删除 $d/"
    fi
  done
  ok "清理完成"
}

# --- 交互菜单 -----------------------------------------------------------------
menu() {
  while true; do
    printf '\n%s\n' "${C_BOLD}════════════ WINOME 构建管理 ════════════${C_RESET}"
    printf '  %s1%s) 生产构建   release + LTO + NDEBUG   %s\n' \
      "$C_BOLD" "$C_RESET" "$(scheme_status release)"
    printf '  %s2%s) 测试构建   debugoptimized (默认)      %s\n' \
      "$C_BOLD" "$C_RESET" "$(scheme_status test)"
    printf '  %s3%s) 调试构建   debug -O0 -g                %s\n' \
      "$C_BOLD" "$C_RESET" "$(scheme_status debug)"
    printf '  %s4%s) 运行 winome (最近构建)\n' "$C_BOLD" "$C_RESET"
    printf '  %s5%s) 清理全部构建目录\n' "$C_BOLD" "$C_RESET"
    printf '  %sq%s) 退出\n' "$C_BOLD" "$C_RESET"

    local choice
    read -rp "选择: " choice || break
    case "$choice" in
      1) do_build release ;;
      2) do_build test ;;
      3) do_build debug ;;
      4) do_run || true ;;
      5)
        read -rp "确认删除所有构建目录? [y/N] " ans
        case "$ans" in
          y|Y) do_clean ;;
          *) warn "已取消" ;;
        esac
        ;;
      q|Q|"") break ;;
      *) warn "无效选择" ;;
    esac
  done
}

usage() {
  sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# --- 入口 ---------------------------------------------------------------------
case "${1:-menu}" in
  release|test|debug) do_build "$1" ;;
  clean) do_clean ;;
  run) do_run ;;
  ""|menu) menu ;;
  -h|--help|help) usage ;;
  *) fail "未知命令: $1"; usage; exit 1 ;;
esac
