// src/core/LayoutEngine.h
// 布局引擎 — macOS 风格鱼眼放大（余弦衰减邻域联动）
// 设计参考：详细设计说明 §2.3
#pragma once
#include "../Common.h"
#include "SpringSystem.h"

// ═══════════════════════════════════════════════
// 图标布局结果
// ═════════════════════════════════════════════
struct IconLayout {
    float x       = 0.0f;   // 主轴中心坐标（Dock 中心为原点；水平=X，竖直=Y）
    float y       = 0.0f;   // 交叉轴偏移（指向屏幕内为正：放大/弹跳时为正）
    float scale   = 1.0f;   // 最终缩放比例
    float opacity = 1.0f;   // 透明度
    int   zIndex  = 0;      // 渲染层级
};

// 弹簧值视图（P0-3）：解耦"图标下标"与"弹簧 id"。DockEngine 每帧从稳定绑定
// m_iconSprings 组装该向量（每帧仅 3×n 次取值，无分配），传给 CalculateLayout 重载。
// rest 布局用常量 {1,0,1} 表示 scale=1/offsetY=0/opacity=1。
struct SpringRead {
    float scale   = 1.0f;   // 当前缩放
    float offsetY = 0.0f;   // 当前交叉轴偏移
    float opacity = 1.0f;   // 当前透明度
};

// ═══════════════════════════════════════════════
// Dock 配置
// ═══════════════════════════════════════════════
struct DockConfig {
    int   iconCount       = 10;
    float baseIconSize    = DockConstants::DEFAULT_ICON_SIZE;
    float maxScale        = DockConstants::DEFAULT_MAX_SCALE;
    float iconSpacing     = DockConstants::DEFAULT_ICON_SPACING;
    float dockPadding     = DockConstants::DEFAULT_PADDING;
    float magnifyRadius   = DockConstants::DEFAULT_MAGNIFY_RADIUS;  // 放大影响半径（图标数）
    float bounceAmplitude = DockConstants::DEFAULT_BOUNCE_AMP;
    DockPosition position = DockPosition::Bottom;
    // #3：上下左右四边「吸附/感应区」独立开关（默认全开，保持旧行为）。
    // 单 dock 仅能停靠一边（position），但各边可独立启用为「允许停靠 + 自动隐藏感应」目标。
    bool edgeTop    = true;
    bool edgeBottom = true;
    bool edgeLeft   = true;
    bool edgeRight  = true;
    // 鱼眼放大特效每边独立开关（默认全开，保持旧行为）。关闭的边仅静态显示图标、
    // 不随鼠标放大（#N：鱼眼无需作用于所有边，可仅对需要的边开启）。
    bool fisheyeTop    = true;
    bool fisheyeBottom = true;
    bool fisheyeLeft   = true;
    bool fisheyeRight  = true;
};

// ═══════════════════════════════════════════════
// 布局引擎
// ═══════════════════════════════════════════════
class LayoutEngine {
public:
    // 计算所有图标的布局（从弹簧系统读取当前动画值）
    void CalculateLayout(
        const DockConfig& config,
        float mouseX,                     // 鼠标X坐标（Dock坐标系，中心为原点）
        bool  isMouseInDock,              // 鼠标是否在Dock区域内
        const SpringSystem& springs,
        std::vector<IconLayout>& outLayouts);

    // 计算所有图标的布局（从弹簧值视图读取，解耦"下标即弹簧 id"，供 P0-3 增量重建）
    void CalculateLayout(
        const DockConfig& config,
        float mouseX,
        bool  isMouseInDock,
        const std::vector<SpringRead>& springs,
        std::vector<IconLayout>& outLayouts);

    // P0-1/2：计算 rest 布局（静息态）。仅计算主轴位置（CalculateXPositions），
    // 逐图标写 scale=1, offsetY=0, opacity=1, zIndex=0。**不接收 SpringSystem**，
    // 彻底零弹簧依赖（Headless 友好，热路径 O(1) 命中）。
    void CalculateRestLayout(const DockConfig& config, std::vector<IconLayout>& outLayouts);

    // 余弦衰减鱼眼目标缩放（供 DockEngine 设置弹簧目标 & 单元测试）
    float CalcTargetScale(int iconIndex, float mouseX, const DockConfig& config) const;

    // 图标静息态中心X（基础尺寸等间距，Dock 中心为原点）
    float GetIconCenterX(int index, const DockConfig& config) const;

private:
    // 计算X坐标（考虑放大后的宽度变化，从中心向两侧展开）
    void CalculateXPositions(const DockConfig& config, std::vector<IconLayout>& layouts) const;
};
