// src/app/DockEngineInternal.h
// DockEngine 拆分用的文件内共享辅助（原 DockEngine.cpp 的 file-local static 提升为
// inline，跨 DockEngine.cpp / DockStateMachine.cpp / DockInteraction.cpp / IconSetManager.cpp
// 四个 TU 共享，零行为变更，避免重复定义 / 链接失败）。
#pragma once
#include "DockEngine.h"   // DockState / DockPosition / RECT 等类型与 WindowManager

// 系统托盘回调消息 & 退出菜单项（原 file-local static constexpr）
inline constexpr UINT WM_APP_TRAY  = WM_APP + 2;
inline constexpr UINT ID_TRAY_EXIT = 1001;

// 窗口消息：动画/重绘心跳 & 看门狗空闲探测。
// 原 DockEngine 私有 static constexpr，T10 拆分后提升为跨 TU 共享 inline，
// 使拆分出的子模块可用裸名（与 WM_APP_TRAY 一致），根除「两类访问风格并存」隐患。
inline constexpr UINT WM_APP_TICK = WM_APP + 1;   // Step 6：动画/重绘心跳
inline constexpr UINT WM_APP_IDLE = WM_APP + 3;   // Step 7：看门狗空闲探测（仅窗口化构建）

// 状态机状态名（诊断日志用）
inline const char* StateName(DockState s) {
    switch (s) {
        case DockState::Hidden:   return "HIDDEN";
        case DockState::Entering: return "ENTERING";
        case DockState::Idle:     return "IDLE";
        case DockState::Hovering: return "HOVERING";
        case DockState::Bouncing: return "BOUNCING";
        case DockState::Exiting:  return "EXITING";
    }
    return "?";
}

// 停靠边名（诊断日志用）
inline const char* PositionName(DockPosition p) {
    switch (p) {
    case DockPosition::Bottom: return "Bottom";
    case DockPosition::Top:    return "Top";
    case DockPosition::Left:   return "Left";
    case DockPosition::Right:  return "Right";
    default:                   return "Unknown";
    }
}

// 取包含给定矩形所属显示器的「工作区」（排除任务栏）。无显示器时回退主工作区。
inline RECT GetMonitorWorkRect(const RECT& r) {
    RECT work = {};
    HMONITOR hmon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    if (hmon) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hmon, &mi)) work = mi.rcWork;
    }
    if (work.right - work.left <= 0 || work.bottom - work.top <= 0) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);   // 兜底：主工作区
    }
    return work;
}
