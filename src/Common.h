// src/Common.h
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <d2d1_3.h>
#include <dcomp.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>

using Microsoft::WRL::ComPtr;

// ═══════════════════════════════════════════════════════════
// 注：DOCK_LOG / DOCK_LOG_WARN / DOCK_LOG_ERR / DOCK_LOG_FRAME 四个
// 调试宏已整体移除——它们仅服务开发期控制台排查。运行期不写任何日志，
// Release 按真实 GUI 路径运行，不保留调试取证日志。
// ═══════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════
// 常量定义
// ═══════════════════════════════════════════════════════════
namespace DockConstants {
    constexpr float DEFAULT_ICON_SIZE      = 48.0f;
    constexpr float DEFAULT_MAX_SCALE      = 2.0f;
    constexpr float DEFAULT_ICON_SPACING   = 8.0f;
    constexpr float DEFAULT_PADDING        = 12.0f;
    constexpr float DEFAULT_MAGNIFY_RADIUS = 3.0f;
    constexpr float DEFAULT_BOUNCE_AMP     = 20.0f;
    constexpr int   TIMER_INTERVAL_MS      = 8;       // ~120fps
    constexpr float DT_MIN                 = 0.001f;
    constexpr float DT_MAX                 = 0.05f;
    constexpr int   SENSE_AREA_EXPAND_PX    = 20;      // 感应区扩展
}

// ═══════════════════════════════════════════════════════════
// 枚举
// ═══════════════════════════════════════════════════════════
enum class DockState {
    Hidden,
    Entering,
    Idle,
    Hovering,
    Bouncing,
    Exiting
};

enum class DockPosition {
    Bottom,
    Top,
    Left,
    Right
};

// ═══════════════════════════════════════════════════════════
// 朝向（布局主/交叉轴）
//   Horizontal：图标沿 X 轴排列（底部/顶部吸附）
//   Vertical  ：图标沿 Y 轴排列（左侧/右侧吸附，竖向长条 Dock）
// magnification 始终朝「屏幕内」（inward）生长：
//   Bottom→上(-Y)  Top→下(+Y)  Left→右(+X)  Right→左(-X)
// ═══════════════════════════════════════════════════════════
enum class DockOrientation { Horizontal, Vertical };

// ═══ 几何函数已迁移至 src/core/EdgeGeometry.h（统一四边几何）═══
// 旧 free function（IsVerticalDock / InwardCrossSign / MapDockLayout /
// ScreenToMainAxis / ComputeDockBarSize）已删除，统一由
//   EdgeGeometry<Orient,RestAtFarEdge,InwardSign> + PositionTraits + IEdgeGeometry
// 经编译期模板参数与运行时多态提供，消除 MapDockLayout 等中的运行时 position 分支。
// 通过 DockEngine / RenderManager / HitTestEngine 持有的 std::unique_ptr<IEdgeGeometry>
// m_geom 委托，并用 MakeGeometry(pos) 作为全工程唯一的运行时 switch。

enum class SpringDimension : uint32_t {
    SCALE     = 0,
    OFFSET_Y  = 1,
    OPACITY   = 2
};

// ═══════════════════════════════════════════════════════════
// 弹簧 ID 编码
// ═══════════════════════════════════════════════════════════
inline uint32_t MakeSpringId(int iconIndex, SpringDimension dim) {
    return static_cast<uint32_t>(iconIndex) * 3 + static_cast<uint32_t>(dim);
}

// ═══════════════════════════════════════════════════════════
// HRESULT 检查宏
// ═══════════════════════════════════════════════════════════
// msg 保留为调用点的自解释错误上下文（编译期丢弃，不产生任何输出）。
#define DOCK_HR_CHECK(hr, msg) \
    do { \
        HRESULT _hr = (hr); \
        if (FAILED(_hr)) { \
            (void)(msg); \
            return _hr; \
        } \
    } while(0)
