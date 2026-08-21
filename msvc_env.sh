#!/usr/bin/env bash
# 手动设置 VS + Windows SDK 环境（替代被沙盒禁止的 vcvarsall.bat / cmd.exe）
#
# 关键：Windows 子进程（cl/ninja/cmake 互相 spawn）需要【反斜杠】路径。
# 注意：双引号内 "\$VAR" 会转义 $ 变成字面量，必须写成 "\\$VAR" 才是"反斜杠+值"。
#
# 本文件不再写死盘符 / VS 版本 / MSVC 工具链版本 / SDK 版本，全部运行时探测：
#   VS 根     : vswhere -> $VSINSTALLDIR -> 遍历 C:/D: 下 Program Files/Microsoft Visual Studio/*/*
#   MSVC 版本 : <VS>/VC/Tools/MSVC/ 下按版本号取最新（sort -V）
#   SDK  版本 : <SDK>/Include/ 下按版本号取最新（sort -V）
#
# 任一探测失败会打印明确错误（缺什么 + 可设哪个环境变量绕过）并 return 1，
# 绝不静默继续 —— 否则 cl.exe 只会报一堆"找不到头文件"，排查成本翻十倍。
#
# 手工覆盖：export VSINSTALLDIR='<VS安装根>'    export WINDOWSSDKDIR='<SDK根>'
#
# 实现注记：本环境没有 cygpath，且部分工具链对反斜杠转义处理不一致，
#           因此路径正/反斜杠转换一律用纯 bash 参数替换，不依赖 sed/tr。

# ── 路径转换助手 ────────────────────────────────────────────────────
# Windows 路径 -> bash 路径：  D:\Foo\Bar  ->  /d/Foo/Bar
_od_to_unix() {
  local p="$1" drv
  p="${p//\\//}"
  case "$p" in
    [A-Za-z]:/*)
      drv="$(printf '%s' "${p:0:1}" | tr '[:upper:]' '[:lower:]')"
      p="/${drv}${p:2}"
      ;;
  esac
  printf '%s' "$p"
}

# bash 路径 -> Windows 路径：  /d/Foo/Bar  ->  D:\Foo\Bar
_od_to_win() {
  local p="$1" drv
  case "$p" in
    /?/*)
      drv="$(printf '%s' "${p:1:1}" | tr '[:lower:]' '[:upper:]')"
      p="${drv}:${p:2}"
      ;;
  esac
  printf '%s' "${p//\//\\}"
}

# 取目录下版本号最大的子目录名（如 14.51.36231 / 10.0.26100.0）
_od_latest_ver() {
  local d out=""
  for d in "$1"/*/; do
    [ -d "$d" ] || continue
    d="${d%/}"
    out="${out}${d##*/}"$'\n'
  done
  [ -n "$out" ] || return 0
  printf '%s' "$out" | sort -V | tail -1
}

# 系统盘（用于定位 Program Files (x86)），不写死 C:
_OD_SYSDRV="${SYSTEMDRIVE:-C:}"
_OD_SYSDRV_L="$(printf '%s' "${_OD_SYSDRV:0:1}" | tr '[:upper:]' '[:lower:]')"
_OD_PFX86="/${_OD_SYSDRV_L}/Program Files (x86)"

# ── 1. 定位 Visual Studio 安装根 ────────────────────────────────────
_OD_VSU=""
_OD_VSWHERE="${_OD_PFX86}/Microsoft Visual Studio/Installer/vswhere.exe"
if [ -f "$_OD_VSWHERE" ]; then
  _OD_RAW="$("$_OD_VSWHERE" -latest -products '*' -property installationPath 2>/dev/null | tr -d '\r' | head -1)"
  if [ -n "$_OD_RAW" ]; then _OD_VSU="$(_od_to_unix "$_OD_RAW")"; fi
fi
if [ -z "$_OD_VSU" ] && [ -n "$VSINSTALLDIR" ]; then
  _OD_VSU="$(_od_to_unix "$VSINSTALLDIR")"
fi
if [ -z "$_OD_VSU" ]; then
  for _OD_D in /c/"Program Files"/"Microsoft Visual Studio"/*/*/ \
               /d/"Program Files"/"Microsoft Visual Studio"/*/*/ ; do
    if [ -d "${_OD_D}VC/Tools/MSVC" ]; then _OD_VSU="${_OD_D%/}"; break; fi
  done
fi
_OD_VSU="${_OD_VSU%/}"

if [ -z "$_OD_VSU" ] || [ ! -d "$_OD_VSU/VC/Tools/MSVC" ]; then
  echo "[msvc_env] ERROR: 找不到 Visual Studio 安装根（需含 VC/Tools/MSVC）。" >&2
  echo "[msvc_env]   已尝试 vswhere : $_OD_VSWHERE" >&2
  echo "[msvc_env]   已尝试 VSINSTALLDIR : ${VSINSTALLDIR:-<未设置>}" >&2
  echo "[msvc_env]   已尝试 目录遍历 : /c|/d/Program Files/Microsoft Visual Studio/*/*" >&2
  echo "[msvc_env]   绕过方式：export VSINSTALLDIR='<VS安装根>' 后重新 source。" >&2
  return 1
fi

# ── 2. 定位 MSVC 工具链版本 ─────────────────────────────────────────
_OD_MSVCVER="$(_od_latest_ver "$_OD_VSU/VC/Tools/MSVC")"
if [ -z "$_OD_MSVCVER" ] || [ ! -d "$_OD_VSU/VC/Tools/MSVC/$_OD_MSVCVER/bin/Hostx64/x64" ]; then
  echo "[msvc_env] ERROR: 在 VS 下找不到可用的 MSVC 工具链（需含 bin/Hostx64/x64）。" >&2
  echo "[msvc_env]   VS 根 : $_OD_VSU" >&2
  echo "[msvc_env]   探测到的 MSVC 版本 : ${_OD_MSVCVER:-<无>}" >&2
  echo "[msvc_env]   请在 VS Installer 中安装「MSVC v14x - VS C++ x64/x86 生成工具」组件。" >&2
  return 1
fi

# ── 3. 定位 Windows SDK 根与版本 ────────────────────────────────────
if [ -n "$WINDOWSSDKDIR" ]; then
  _OD_SDKU="$(_od_to_unix "$WINDOWSSDKDIR")"
else
  _OD_SDKU="${_OD_PFX86}/Windows Kits/10"
fi
_OD_SDKU="${_OD_SDKU%/}"

_OD_SDKVER="$(_od_latest_ver "$_OD_SDKU/Include")"
if [ -z "$_OD_SDKVER" ] || [ ! -d "$_OD_SDKU/Include/$_OD_SDKVER/ucrt" ]; then
  echo "[msvc_env] ERROR: 找不到 Windows SDK（需含 Include/<版本>/ucrt）。" >&2
  echo "[msvc_env]   SDK 根 : $_OD_SDKU" >&2
  echo "[msvc_env]   探测到的 SDK 版本 : ${_OD_SDKVER:-<无>}" >&2
  echo "[msvc_env]   绕过方式：export WINDOWSSDKDIR='<SDK根，如 C:\\Program Files (x86)\\Windows Kits\\10>'" >&2
  return 1
fi

# ── 4. 转成 Windows 反斜杠路径 ──────────────────────────────────────
VSW="$(_od_to_win "$_OD_VSU")"
MSVCW="$(_od_to_win "$_OD_VSU/VC/Tools/MSVC/$_OD_MSVCVER")"
SDKW="$(_od_to_win "$_OD_SDKU")"
SDKVER="$_OD_SDKVER"

export MSVC_BIN="$MSVCW\bin\Hostx64\x64"
export CMAKE_EXE="$VSW\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
export NINJA_EXE="$VSW\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
export SDK_BIN="$SDKW\bin\\$SDKVER\x64"

# Windows 风格（反斜杠 + 分号）PATH，仅供 cl.exe / ninja / cmake 子进程使用
# 必须包含 System32：调试版 EXE 依赖 ucrtbased.dll 等系统调试 CRT，
# ctest 派生的子进程仅继承此 PATH，缺 System32 会导致“Process not started”。
# 注意用 SYSTEMDRIVE 而非 SYSTEMROOT：后者在本机是 C:\WINDOWS（全大写），
# 会让 PATH 与历史值不一致。
export WINPATH="$MSVC_BIN;$SDK_BIN;$VSW\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$VSW\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$_OD_SYSDRV\Windows\System32"

# 头文件 / 库（反斜杠，不影响 bash）
export INCLUDE="$MSVCW\include;$SDKW\Include\\$SDKVER\um;$SDKW\Include\\$SDKVER\shared;$SDKW\Include\\$SDKVER\winrt;$SDKW\Include\\$SDKVER\ucrt"
export LIB="$MSVCW\lib\x64;$SDKW\Lib\\$SDKVER\um\x64;$SDKW\Lib\\$SDKVER\ucrt\x64"

# 清理内部符号，避免污染调用方 shell
unset -f _od_to_unix _od_to_win _od_latest_ver
unset _OD_SYSDRV _OD_SYSDRV_L _OD_PFX86 _OD_VSU _OD_VSWHERE _OD_RAW _OD_D _OD_MSVCVER _OD_SDKU _OD_SDKVER

echo "[env] CMAKE_EXE = $CMAKE_EXE"
echo "[env] WINPATH   = $WINPATH"
echo "[env] INCLUDE   = $INCLUDE"
