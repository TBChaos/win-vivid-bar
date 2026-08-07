// src/core/LayoutEngine.cpp
#include "LayoutEngine.h"

void LayoutEngine::CalculateLayout(
    const DockConfig& config,
    float mouseX,
    bool  isMouseInDock,
    const SpringSystem& springs,
    std::vector<IconLayout>& outLayouts)
{
    (void)mouseX;
    (void)isMouseInDock;
    outLayouts.resize(static_cast<size_t>(config.iconCount));

    // 第一步：从弹簧系统读取每个图标的当前动画值
    for (int i = 0; i < config.iconCount; ++i) {
        uint32_t scaleSpringId   = MakeSpringId(i, SpringDimension::SCALE);
        uint32_t bounceSpringId  = MakeSpringId(i, SpringDimension::OFFSET_Y);
        uint32_t opacitySpringId = MakeSpringId(i, SpringDimension::OPACITY);

        float currentScale = springs.GetValue(scaleSpringId);
        float bounceOffset = springs.GetValue(bounceSpringId);
        float opacity      = springs.GetValue(opacitySpringId);

        IconLayout& L = outLayouts[static_cast<size_t>(i)];
        L.scale   = currentScale;
        L.opacity = opacity;
        L.zIndex  = (currentScale > 1.01f) ? 1 : 0;

        // 交叉轴偏移：指向屏幕内为正（Bottom→上 / Top→下 / Left→右 / Right→左）
        float scaleOffset = (currentScale - 1.0f) * config.baseIconSize * 0.5f;
        L.y = bounceOffset + scaleOffset;
    }

    // 第二步：计算X坐标（考虑放大后的宽度变化）
    CalculateXPositions(config, outLayouts);
}

void LayoutEngine::CalculateLayout(
    const DockConfig& config,
    float mouseX,
    bool  isMouseInDock,
    const std::vector<SpringRead>& springs,
    std::vector<IconLayout>& outLayouts)
{
    (void)mouseX;
    (void)isMouseInDock;
    outLayouts.resize(springs.size());

    // 第一步：从弹簧值视图向量读取每个图标的当前动画值（与 SpringSystem 重载语义一致）
    for (size_t i = 0; i < springs.size(); ++i) {
        float currentScale = springs[i].scale;
        float bounceOffset = springs[i].offsetY;
        float opacity      = springs[i].opacity;

        IconLayout& L = outLayouts[i];
        L.scale   = currentScale;
        L.opacity = opacity;
        L.zIndex  = (currentScale > 1.01f) ? 1 : 0;

        // 交叉轴偏移：指向屏幕内为正（Bottom→上 / Top→下 / Left→右 / Right→左）
        float scaleOffset = (currentScale - 1.0f) * config.baseIconSize * 0.5f;
        L.y = bounceOffset + scaleOffset;
    }

    // 第二步：计算X坐标（考虑放大后的宽度变化）
    CalculateXPositions(config, outLayouts);
}

void LayoutEngine::CalculateRestLayout(const DockConfig& config,
                                       std::vector<IconLayout>& outLayouts)
{
    outLayouts.resize(static_cast<size_t>(config.iconCount));

    // 静息态：scale=1, offsetY/y=0, opacity=1, zIndex=0（与原「重建 rest 弹簧 + CalculateLayout」
    // 输入恒为 (1,0,1) 的结果逐像素一致）。零弹簧依赖，可 Headless 单测。
    for (auto& L : outLayouts) {
        L.scale   = 1.0f;
        L.y       = 0.0f;
        L.opacity = 1.0f;
        L.zIndex  = 0;
    }

    // 仅计算主轴坐标（考虑间距与基础尺寸，从中心向两侧展开）
    CalculateXPositions(config, outLayouts);
}

float LayoutEngine::CalcTargetScale(int iconIndex, float mouseX, const DockConfig& config) const {
    // 图标中心到鼠标的距离（以图标数为单位）
    float iconCenterX = GetIconCenterX(iconIndex, config);
    float distanceInIcons = std::abs(mouseX - iconCenterX)
                            / (config.baseIconSize + config.iconSpacing);

    if (distanceInIcons >= config.magnifyRadius) {
        return 1.0f;  // 超出影响范围
    }

    // 余弦衰减：t ∈ [0, 1]，0=正下方，1=影响边界
    float t = distanceInIcons / config.magnifyRadius;
    float cosineFactor = 0.5f * (1.0f + std::cos(t * 3.14159265f));

    return 1.0f + (config.maxScale - 1.0f) * cosineFactor;
}

float LayoutEngine::GetIconCenterX(int index, const DockConfig& config) const {
    // 基于基础尺寸的等间距静息位置（Dock 中心为原点）
    return (static_cast<float>(index) - (config.iconCount - 1) * 0.5f)
           * (config.baseIconSize + config.iconSpacing);
}

void LayoutEngine::CalculateXPositions(const DockConfig& config, std::vector<IconLayout>& layouts) const {
    float totalWidth = 0.0f;

    // 先计算总宽度
    for (int i = 0; i < config.iconCount; ++i) {
        totalWidth += config.baseIconSize * layouts[static_cast<size_t>(i)].scale + config.iconSpacing;
    }
    totalWidth -= config.iconSpacing;  // 去掉最后一个间距

    // 从中心向两侧展开
    float startX   = -totalWidth * 0.5f;
    float currentX = startX;

    for (int i = 0; i < config.iconCount; ++i) {
        IconLayout& L = layouts[static_cast<size_t>(i)];
        float iconWidth = config.baseIconSize * L.scale;
        L.x = currentX + iconWidth * 0.5f;
        currentX += iconWidth + config.iconSpacing;
    }
}
