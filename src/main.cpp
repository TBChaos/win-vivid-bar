// src/main.cpp
// openDock 进程入口（Release / GUI 窗口子系统）。
//
// 历史上本文件还承载自测与集成验证套件（约 2700 行验证函数），以及无头仿真驱动块。
// 二者已整体移除，现在这里只保留真实运行期的启动链路：
// DPI 感知 → 命令行开关 → DockManager 启动。
#include "app/DockManager.h"     // #4 升级：四边同时显示（多 Dock 编排）
#include <windows.h>
#include <cwchar>                // wcsstr（命令行开关探测）

// 生产配置路径。相对路径由 DockManager::InitializeFromFile 依 exe 目录解析
//（见 PathUtil::ResolveConfigPath 的候选回退链）。
static const char* const kConfigPath = "res/config.json";

// --force-gdi：强制走 GDI 三级降级回退，用于排查 DComp/D3D 不可用的机器。
static bool HasForceGdiFlag() {
    return wcsstr(GetCommandLineW(), L"--force-gdi") != nullptr;
}

// 需求 7：开机自启拉起标记。AutoStart::Enable 写入 Run 键时恒带 --autostart，
// 进程据此区分「用户手动启动」与「开机拉起」，后者延后建窗（见 DockMain）。
static bool HasAutoStartFlag() {
    return wcsstr(GetCommandLineW(), L"--autostart") != nullptr;
}

// Step 6：进程级 Per-Monitor V2 DPI 感知（Win10 1703+，动态加载兼容旧系统）
static void EnablePerMonitorDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI* PFN_SetCtx)(HANDLE);
    auto p = user32
        ? reinterpret_cast<PFN_SetCtx>(
              (void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
        : nullptr;
    if (p) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4
        p(reinterpret_cast<HANDLE>(-4));
    }
}

int DockMain() {
    EnablePerMonitorDpiAwareness();

    // 需求 7：开机拉起时 explorer.exe 可能尚未就绪，此刻取 SHAppBarMessage/rcWork 会拿到
    // 错误工作区，导致 Dock 定位偏移。延后 2s 再建窗口；手动启动不延迟。
    if (HasAutoStartFlag()) {
        Sleep(2000);
    }

    // 多 Dock（#4 升级：四边同时显示）——每条启用的边一个独立 Dock 栏，各自图标集 /
    // 悬停放大 / 自动隐藏 / 拖放 / 右键菜单；由 DockManager 统一运行单一消息循环
    // 并持有单个系统托盘入口。
    DockManager mgr;
    // 须在 InitializeFromFile 之前下发：各边引擎在 CreateEdgeEngine 内建立，
    // 强制回退标志要赶在 DockEngine::Initialize 把它交给 RenderManager 之前设好。
    mgr.SetForceGdiFallback(HasForceGdiFlag());
    if (FAILED(mgr.InitializeFromFile(kConfigPath))) {
        return 1;
    }
    mgr.ShowAll();
    mgr.Run();
    mgr.Shutdown();
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return DockMain(); }
