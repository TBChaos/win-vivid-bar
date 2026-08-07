// src/core/EdgeGeometry.h
// 统一配置 C++ 模块 —— 四边（Top/Right/Bottom/Left）统一几何
// 设计要点：
//   * 用「方向标签 + constexpr 模板参数」把布局方向提到编译期，
//     消除 MapDockLayout/ComputeDockBarSize/ScreenToMainAxis 中的运行时
//     switch/if(!IsVerticalDock(pos)) 分支（汇编里无 position 判断）。
//   * EdgeGeometry<Orient, RestAtFarEdge, InwardSign> 为唯一几何真源；
//     PositionTraits<DockPosition> 把 DockPosition 一次性编译期映射到参数；
//     IEdgeGeometry / EdgeGeometryImpl 提供多态接口（继承 + 多态）；
//     MakeGeometry 是全工程唯一的运行时 switch（位置 → 多态几何实例）。
// ══════════════════════════════════════════════════════════════════════
#pragma once
#include "../Common.h"
#include "LayoutEngine.h"   // IconLayout（iconVisualRect 的输入）
#include <type_traits>
#include <memory>

// ══════════════════════════════════════════════════════════════════════
// ADR §1.5.2：单一真源 iconVisualRect 所需的值类型
//   RectF             —— 轴对齐浮点矩形（Dock 局部坐标，left/top 原点=基础条左上角）
//   IconGeometryParams—— 图标几何求解的输入参数包（一次装配，四处消费）
// ══════════════════════════════════════════════════════════════════════
struct IconGeometryParams {
    float dockW        = 0.0f;   // Dock 基础条宽（含 padding）
    float dockH        = 0.0f;   // Dock 基础条高（含 padding）
    float baseIconSize = 0.0f;   // b
    float dockPadding  = 0.0f;   // p
};

struct RectF {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    float cx() const { return (left + right) * 0.5f; }
    float cy() const { return (top + bottom) * 0.5f; }
    float w()  const { return right - left; }
    float h()  const { return bottom - top; }
    bool  contains(float x, float y) const {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
    RectF inflated(float d) const { return { left - d, top - d, right + d, bottom + d }; }
};

// ══════════════════════════════════════════════════════════════════════
// 方向标签（布局主/交叉轴）
//   Horizontal：图标沿 X 轴排列（底部/顶部吸附）
//   Vertical  ：图标沿 Y 轴排列（左侧/右侧吸附，竖向长条 Dock）
// ══════════════════════════════════════════════════════════════════════
struct Horizontal {};
struct Vertical   {};

// ══════════════════════════════════════════════════════════════════════
// EdgeGeometry<Orient, RestAtFarEdge, InwardSign>：四边统一几何，零运行时 position 分支
//   Orient            : Horizontal/Vertical 标签（用户要求的"方向模板参数"）
//   RestAtFarEdge     : 静止时图标贴"远边"(true)还是"近边"(false)。
//                       Bottom/Right=true, Top/Left=false
//   InwardSign        : 放大朝屏幕内的符号。Bottom=-1, Top=+1, Left=+1, Right=-1
// 注：原 MapDockLayout 中 Top 与 Bottom、Left 与 Right 不仅 inward 符号不同，
//     静止贴边基准也不同，故必须有 RestAtFarEdge 这一 constexpr 参数，否则像素对不上。
// ══════════════════════════════════════════════════════════════════════
template <typename Orient, bool RestAtFarEdge, int InwardSign>
struct EdgeGeometry {
    static constexpr bool isVertical = std::is_same_v<Orient, Vertical>;

    // 规范布局坐标 → Dock 局部屏幕坐标（left=0, top=0 的图标中心）
    static inline void MapLayout(float mainX, float cross,
                                 float dockW, float dockH,
                                 float baseIconSize, float dockPadding,
                                 float& outX, float& outY) {
        if constexpr (isVertical) {
            outY = dockH * 0.5f + mainX;
            float restX = RestAtFarEdge ? (dockW - dockPadding - baseIconSize * 0.5f)
                                        : (dockPadding + baseIconSize * 0.5f);
            outX = restX + cross * static_cast<float>(InwardSign);
        } else {
            outX = dockW * 0.5f + mainX;
            float restY = RestAtFarEdge ? (dockH - dockPadding - baseIconSize * 0.5f)
                                        : (dockPadding + baseIconSize * 0.5f);
            outY = restY + cross * static_cast<float>(InwardSign);
        }
    }

    // Dock 局部坐标 → 主轴中心坐标（鼠标映射到放大轴）
    static inline float ScreenToMainAxis(float localX, float localY,
                                         float dockW, float dockH) {
        if constexpr (isVertical) return localY - dockH * 0.5f;
        else                      return localX - dockW * 0.5f;
    }

    // 计算 Dock 条（基础条）尺寸；竖直朝向交换宽高
    static inline void ComputeBarSize(int iconCount, float baseIconSize,
                                      float iconSpacing, float dockPadding,
                                      float& outW, float& outH) {
        float along  = iconCount * baseIconSize
                     + (iconCount > 0 ? (iconCount - 1) * iconSpacing : 0.0f)
                     + dockPadding * 2.0f;
        float across = baseIconSize + dockPadding * 2.0f;
        if constexpr (isVertical) { outW = across; outH = along; }
        else                      { outW = along;  outH = across; }
    }

    // 反算 MapLayout：Dock 局部坐标(outX,outY) → 规范布局(mainX,cross)
    // 用于拖拽时让被拖图标跟随光标（#4）。数学逆，符号与 MapLayout 严格一致。
    static inline void InverseMapLayout(float outX, float outY,
                                        float dockW, float dockH,
                                        float baseIconSize, float dockPadding,
                                        float& mainX, float& cross) {
        if constexpr (isVertical) {
            mainX = outY - dockH * 0.5f;
            float restX = RestAtFarEdge ? (dockW - dockPadding - baseIconSize * 0.5f)
                                        : (dockPadding + baseIconSize * 0.5f);
            cross = (outX - restX) / static_cast<float>(InwardSign);
        } else {
            mainX = outX - dockW * 0.5f;
            float restY = RestAtFarEdge ? (dockH - dockPadding - baseIconSize * 0.5f)
                                        : (dockPadding + baseIconSize * 0.5f);
            cross = (outY - restY) / static_cast<float>(InwardSign);
        }
    }
};

// ══════════════════════════════════════════════════════════════════════
// PositionTraits<DockPosition>：DockPosition -> (Orient,RestAtFarEdge,InwardSign)
// 的编译期映射（全工程"方向"的唯一真源；运行时 switch 只在 MakeGeometry 出现一次）
// ══════════════════════════════════════════════════════════════════════
template <DockPosition P> struct PositionTraits;
template <> struct PositionTraits<DockPosition::Bottom> { using Orient = Horizontal; static constexpr bool RestAtFarEdge = true;  static constexpr int InwardSign = -1; };
template <> struct PositionTraits<DockPosition::Top>    { using Orient = Horizontal; static constexpr bool RestAtFarEdge = false; static constexpr int InwardSign = +1; };
template <> struct PositionTraits<DockPosition::Left>   { using Orient = Vertical;   static constexpr bool RestAtFarEdge = false; static constexpr int InwardSign = +1; };
template <> struct PositionTraits<DockPosition::Right>  { using Orient = Vertical;   static constexpr bool RestAtFarEdge = true;  static constexpr int InwardSign = -1; };

// ══════════════════════════════════════════════════════════════════════
// 多态几何接口（继承 + 多态）：DockEngine/RenderManager/HitTestEngine 持有并委托
// ══════════════════════════════════════════════════════════════════════
class IEdgeGeometry {
public:
    virtual ~IEdgeGeometry() = default;
    virtual void mapLayout(float mainX, float cross, float dockW, float dockH,
                           float baseIconSize, float dockPadding,
                           float& outX, float& outY) const = 0;
    virtual float screenToMainAxis(float localX, float localY, float dockW, float dockH) const = 0;
    virtual void computeBarSize(int iconCount, float baseIconSize, float iconSpacing,
                                float dockPadding, float& outW, float& outH) const = 0;
    virtual void inverseMap(float outX, float outY, float dockW, float dockH,
                            float baseIconSize, float dockPadding,
                            float& mainX, float& cross) const = 0;
    virtual bool isVertical() const = 0;

    // ★ ADR §1.5.2 单一真源：图标的【可见矩形】（Dock 局部坐标，浮点，未取整）。
    //   契约（INV-VISUAL）：
    //     1) 渲染器绘制的像素范围 ≡ 本函数返回值（DComp 与 GDI 两条路径都必须由它推导）；
    //     2) 命中判定的基准矩形 ≡ 本函数返回值；
    //     3) 中心 == mapLayout 得到的 (cx, cy)，半边长 == baseIconSize*L.scale/2（各向同性）；
    //     4) 四边等价：见 ADR §1.2 定理（EdgeGeometry 模板参数化，无逐边分支）。
    //   纯函数、无状态、可 Headless 单测。
    virtual RectF iconVisualRect(const IconLayout& layout,
                                 const IconGeometryParams& gp) const = 0;

    // ★ 命中矩形 = 可见矩形 ⊕ expandPx（各向同性）。独立成非虚函数是为了让
    //   「膨胀量从哪来」只有一个答案（DockConstants::SENSE_AREA_EXPAND_PX）。
    RectF iconHitRect(const IconLayout& layout,
                      const IconGeometryParams& gp,
                      float expandPx) const {
        return iconVisualRect(layout, gp).inflated(expandPx);
    }
};

template <typename Orient, bool RestAtFarEdge, int InwardSign>
class EdgeGeometryImpl : public IEdgeGeometry {
    using G = EdgeGeometry<Orient, RestAtFarEdge, InwardSign>;
public:
    void mapLayout(float mainX, float cross, float dockW, float dockH,
                   float baseIconSize, float dockPadding,
                   float& outX, float& outY) const override {
        G::MapLayout(mainX, cross, dockW, dockH, baseIconSize, dockPadding, outX, outY);
    }
    float screenToMainAxis(float localX, float localY, float dockW, float dockH) const override {
        return G::ScreenToMainAxis(localX, localY, dockW, dockH);
    }
    void computeBarSize(int iconCount, float baseIconSize, float iconSpacing,
                        float dockPadding, float& outW, float& outH) const override {
        G::ComputeBarSize(iconCount, baseIconSize, iconSpacing, dockPadding, outW, outH);
    }
    void inverseMap(float outX, float outY, float dockW, float dockH,
                    float baseIconSize, float dockPadding,
                    float& mainX, float& cross) const override {
        G::InverseMapLayout(outX, outY, dockW, dockH, baseIconSize, dockPadding, mainX, cross);
    }
    bool isVertical() const override { return G::isVertical; }

    // 单一真源实现：零运行时分支，四边差异全部由 EdgeGeometry<> 模板参数承担。
    RectF iconVisualRect(const IconLayout& L, const IconGeometryParams& gp) const override {
        float cx = 0.0f, cy = 0.0f;
        G::MapLayout(L.x, L.y, gp.dockW, gp.dockH, gp.baseIconSize, gp.dockPadding, cx, cy);
        const float h = gp.baseIconSize * L.scale * 0.5f;
        return { cx - h, cy - h, cx + h, cy + h };
    }
};

// MakeGeometry：全工程唯一的运行时 switch（位置 -> 多态几何实例）
inline std::unique_ptr<IEdgeGeometry> MakeGeometry(DockPosition p) {
    switch (p) {
    case DockPosition::Bottom: return std::make_unique<EdgeGeometryImpl<Horizontal, true,  -1>>();
    case DockPosition::Top:    return std::make_unique<EdgeGeometryImpl<Horizontal, false, +1>>();
    case DockPosition::Left:   return std::make_unique<EdgeGeometryImpl<Vertical,   false, +1>>();
    case DockPosition::Right:  return std::make_unique<EdgeGeometryImpl<Vertical,   true,  -1>>();
    }
    return std::make_unique<EdgeGeometryImpl<Horizontal, true, -1>>();
}
