// src/core/HitTestEngine.h
// 命中测试引擎 — 悬停检测 / Dock 感应区判定
// 设计参考：详细设计说明 §2.5（Step 14：支持水平/竖直两种朝向）
#pragma once
#include "../Common.h"
#include "LayoutEngine.h"
#include "EdgeGeometry.h"   // 统一四边几何（IEdgeGeometry / MakeGeometry）

class HitTestEngine {
public:
    struct HitResult {
        bool  isInDock     = false;   // 鼠标是否在Dock区域内（含感应区）
        int   hoveredIndex = -1;      // 悬停的图标索引（-1=无）
        float mouseMainInDock = 0.0f; // 鼠标在主轴（Dock 局部）上的坐标（中心为原点）
    };

    // screenPos:  屏幕坐标
    // dockRect:   Dock 基础条屏幕矩形
    // layouts:    图标规范布局（x=主轴中心坐标, y=指向屏幕内的交叉偏移）
    // pos/dockWidth/dockHeight/baseIconSize/dockPadding：用于把规范布局映射到 Dock 局部坐标
    // senseExpandPx：命中矩形的各向同性膨胀量。
    //   ⚠ ADR §1.5.4：【不得提供默认值】。历史缺陷 D3 正是因为这里默认 0，
    //   HandleMouseMove 忘了传参用 0、HitTestAt 传 20 —— 于是「系统认为能点」
    //   与「程序认为命中」的集合永不相等，中间夹着一圈死区。去掉默认值后，
    //   每个调用点都必须在编译期显式表态，物理上杜绝复发。
    //   生产调用点一律传 DockConstants::SENSE_AREA_EXPAND_PX。
    HitResult Test(
        POINT screenPos,
        const RECT& dockRect,
        const std::vector<IconLayout>& layouts,
        DockPosition pos,
        float dockWidth,
        float dockHeight,
        float baseIconSize,
        float dockPadding,
        float senseExpandPx) const;

    // P0-4：四角→IDLE 区域判定（《动作执行规范》§2 硬约束）。屏幕四角为相邻两边
    // 感应带法向厚度交叠正方形，光标落于角格不得唤出任何 Dock。零 OS 依赖、可 Headless 单测。
    // workArea=当前显示器工作区；cornerPx=角格边长（相邻两带法向厚度交叠正方形，取较小带厚）。
    static bool IsInCornerIdleZone(POINT pt, RECT workArea, int cornerPx);

private:
    mutable std::unique_ptr<IEdgeGeometry> m_geom;   // 统一四边几何（编译期模板 + 多态）；
                                                     // mutable：Test 为 const 查询，惰性缓存几何实例
    mutable DockPosition m_lastPos{ DockPosition::Bottom };  // 自愈：记录上次 Test 使用的边，
                                                             // 边变化时重建 m_geom，不依赖调用方同步
};
