#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════
# openDock 构建脚本（替代被沙盒禁止的 rebuild.bat —— 那个会调 cmd.exe）
#
# 仅保留发布形态：
#   ./build.sh              正式版  → build/      Release（GUI 窗口子系统）
#
# 用法：
#   ./build.sh                      正式版：configure + build
#   ./build.sh --no-config          正式版：仅增量 build（跳过 configure）
#   ./build.sh --clean              先删构建目录，再全新构建（pristine）
#
# 设计铁律（去调试化后）：
#   1. 不再提供 debug 构建链路；CMakeLists.txt 已移除调试模式选项，
#      恒为 Windows 子系统 + /O2 优化，诊断日志与无头仿真驱动整体移除。
#   2. 构建目录固定为 build/，杜绝多形态 CMakeCache 串味。
#   3. 构建结束必须打印实际生效的形态。
# ══════════════════════════════════════════════════════════════════════
set -e
cd "$(dirname "$0")"
source ./msvc_env.sh

# ---------- 解析参数 ----------
NO_CONFIG=0
CLEAN=0
for a in "$@"; do
  case "$a" in
    --no-config) NO_CONFIG=1 ;;
    --clean)     CLEAN=1 ;;
    -h|--help)
      sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "[warn] 未知参数已忽略: $a" ;;
  esac
done

BUILD_DIR="build"
BUILD_TYPE="Release"
FLAVOR="正式版（发布形态 / GUI 窗口子系统）"

echo ""
echo "════════════════════════════════════════════════════"
echo " openDock 构建：$FLAVOR"
echo "   目录            = $BUILD_DIR/"
echo "   CMAKE_BUILD_TYPE= $BUILD_TYPE"
echo "════════════════════════════════════════════════════"

# ---------- 可选：pristine 清理 ----------
if [ "$CLEAN" -eq 1 ]; then
  echo "=== clean: 移除 $BUILD_DIR/ ==="
  rm -rf "$(pwd -W 2>/dev/null || pwd)/$BUILD_DIR"
  NO_CONFIG=0   # 清过之后必须重新 configure
fi

# 构建目录不存在时强制 configure，避免 --no-config 误用
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  NO_CONFIG=0
fi

# ---------- configure ----------
if [ "$NO_CONFIG" -eq 0 ]; then
  echo "=== configure ($BUILD_DIR) ==="
  PATH="$WINPATH" "$CMAKE_EXE" -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_MAKE_PROGRAM="$NINJA_EXE" \
    -DCMAKE_CXX_COMPILER="$MSVC_BIN\cl.exe" \
    -DCMAKE_C_COMPILER="$MSVC_BIN\cl.exe"
else
  echo "=== 跳过 configure（--no-config，沿用 $BUILD_DIR/CMakeCache.txt）==="
fi

# ---------- build ----------
echo "=== build ($BUILD_DIR) ==="
PATH="$WINPATH" "$CMAKE_EXE" --build "$BUILD_DIR"

# ---------- 形态自检 ----------
ACTUAL_TYPE=$(grep -E '^CMAKE_BUILD_TYPE:STRING=' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2)

echo ""
echo "════════════════════════════════════════════════════"
echo " 构建完成：$BUILD_DIR/openDock.exe"
echo "   实际生效 CMAKE_BUILD_TYPE = ${ACTUAL_TYPE:-<未知>}"
echo "   ✅ 这是【正式版】，可交付给用户（GUI 窗口，无控制台）"
echo "════════════════════════════════════════════════════"
