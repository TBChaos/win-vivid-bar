"""
msvc_env.py - derive MSVC toolchain + Windows SDK environment for openDock build.

This avoids calling the (often broken on some machines) VsDevCmd.bat /
vcvarsall.bat. Instead it scans the on-disk layout for the newest MSVC
toolchain and the newest Windows SDK, then writes a small `set`-env batch
file that build.bat `call`s.

Usage:
    python msvc_env.py <vs_install_dir> <output_bat_path> [debug]

Exit codes:
    0  success (env file written)
    2  bad arguments
    3  no MSVC toolchain found
    4  no Windows SDK found
"""
import os
import sys
import glob


def newest_in(parent):
    """Return the alphabetically-largest immediate subdir name under parent, or ''."""
    if not parent or not os.path.isdir(parent):
        return ""
    names = [os.path.basename(p) for p in glob.glob(os.path.join(parent, "*"))]
    names = [n for n in names if os.path.isdir(os.path.join(parent, n))]
    return sorted(names)[-1] if names else ""


def find_windows_sdk_root():
    """Locate Windows Kits\\10 by scanning common ProgramFiles(x86) roots."""
    roots = []
    pf = os.environ.get("ProgramFiles(x86)")
    if pf:
        roots.append(pf)
    for drive in ("C", "D", "E"):
        roots.append("%s:\\Program Files (x86)" % drive)
    seen = set()
    include_dirs = []
    for base in roots:
        if not base or base in seen:
            continue
        seen.add(base)
        p = os.path.join(base, "Windows Kits", "10", "Include")
        if os.path.isdir(p):
            include_dirs.append(p)
    if not include_dirs:
        return None, None
    sdk = newest_in(include_dirs[0])
    if not sdk:
        return None, None
    # include_dirs[0] == <root>\Windows Kits\10\Include
    # we want sdk_root = <root>\Windows Kits\10  (ONE level up, not two!)
    # BUG-2026-08-05: a previous version went two levels up, dropping the
    # "\10\" segment, so INCLUDE/LIB pointed at "...\Windows Kits\Include\..."
    # which does not exist -> cl.exe could not find windows.h / stdio.h.
    sdk_root = os.path.dirname(include_dirs[0])
    return sdk_root, sdk


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: msvc_env.py <vs_install_dir> <output_bat> [debug]\n")
        return 2

    vs = sys.argv[1]
    out = sys.argv[2]

    if not os.path.isdir(vs):
        sys.stderr.write("VS install dir not found: %s\n" % vs)
        return 2

    msvc_root = os.path.join(vs, "VC", "Tools", "MSVC")
    msvc = newest_in(msvc_root)
    if not msvc:
        sys.stderr.write("NO_MSVC_TOOLCHAIN under %s\n" % msvc_root)
        return 3
    msvc_bin = os.path.join(msvc_root, msvc, "bin", "Hostx64", "x64")

    sdk_root, sdk = find_windows_sdk_root()
    if not sdk_root or not sdk:
        sys.stderr.write("NO_WINDOWS_SDK found under common ProgramFiles(x86)\n")
        return 4

    cmake_dir = os.path.join(vs, "Common7", "IDE", "CommonExtensions",
                             "Microsoft", "CMake", "CMake", "bin")
    ninja_dir = os.path.join(vs, "Common7", "IDE", "CommonExtensions",
                             "Microsoft", "CMake", "Ninja")

    inc = ";".join([
        os.path.join(msvc_root, msvc, "include"),
        os.path.join(sdk_root, "Include", sdk, "um"),
        os.path.join(sdk_root, "Include", sdk, "shared"),
        os.path.join(sdk_root, "Include", sdk, "winrt"),
        os.path.join(sdk_root, "Include", sdk, "ucrt"),
    ])
    lib = ";".join([
        os.path.join(msvc_root, msvc, "lib", "x64"),
        os.path.join(sdk_root, "Lib", sdk, "um", "x64"),
        os.path.join(sdk_root, "Lib", sdk, "ucrt", "x64"),
    ])
    path_extra = ";".join([
        msvc_bin,
        os.path.join(sdk_root, "bin", sdk, "x64"),
        cmake_dir,
        ninja_dir,
        os.path.join(os.environ.get("SystemRoot", "C:\\WINDOWS"), "System32"),
    ])

    # --- sanity: confirm the SDK/MSVC headers actually resolve ---
    # crtdbg.h is a DEBUG-ONLY CRT header (guarded by _DEBUG). It is never
    # included in Release builds, so flagging its absence there is misleading.
    # Only assert its presence when an explicit debug build is requested.
    debug_build = len(sys.argv) > 3 and sys.argv[3].lower() in ("1", "debug", "on", "true")
    win_h = os.path.join(sdk_root, "Include", sdk, "um", "windows.h")
    crt_h = os.path.join(msvc_root, msvc, "include", "crtdbg.h")
    sys.stderr.write("[CHECK] windows.h -> %s : %s\n" % (win_h, "OK" if os.path.isfile(win_h) else "MISSING"))
    if debug_build:
        sys.stderr.write("[CHECK] crtdbg.h  -> %s : %s\n" % (crt_h, "OK" if os.path.isfile(crt_h) else "MISSING"))
    else:
        sys.stderr.write("[CHECK] crtdbg.h  -> n/a (Release build, debug-only header)\n")
    orig_path = os.environ.get("PATH", "")

    lines = []
    lines.append("@echo off")
    lines.append('set "MSVCDIR=%s"' % msvc)
    lines.append('set "SDKVER=%s"' % sdk)
    lines.append('set "VCINSTALLDIR=%s\\"' % os.path.join(vs, "VC"))
    lines.append('set "VCToolsInstallDir=%s\\"' % os.path.join(msvc_root, msvc))
    lines.append('set "WindowsSdkDir=%s\\"' % sdk_root)
    lines.append('set "WindowsSDKVersion=%s\\"' % sdk)
    lines.append('set "UCRTVersion=%s"' % sdk)
    lines.append('set "INCLUDE=%s"' % inc)
    lines.append('set "LIB=%s"' % lib)
    lines.append('set "PATH=%s;%s"' % (path_extra, orig_path))

    with open(out, "w", encoding="ascii", newline="\r\n") as f:
        f.write("\r\n".join(lines) + "\r\n")

    sys.stderr.write("MSVC %s / SDK %s -> %s\n" % (msvc, sdk, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
