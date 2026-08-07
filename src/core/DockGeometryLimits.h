// src/core/DockGeometryLimits.h
// 包络不变量（INV-ENVELOPE）反解所需的可达集上界常量。
//
// 设计要点（ADR_P3_design.md §1.3）：
//   * 图标的 scale / bounce 由二阶弹簧驱动，会过冲（overshoot）与回弹（undershoot）。
//     窗口留白（inset）必须覆盖【可达集】而非【目标值】，否则满放大/回弹瞬间图标
//     会溢出 OS 窗口矩形 —— 溢出的那部分像素永远收不到 WM_NCHITTEST，
//     表现为「图标看得见但点不到」（缺陷 D1/D6/D7）。
//   * 这里的四个系数是对 SpringParams::Hover()/Bounce() 的数值反解上界，
//     严格覆盖 bang-bang 最坏值。**改弹簧参数必须重跑推导并同步这里。**
//   * 常量集中在本文件，ComputeInsets 只做公式，不再出现任何手改的魔数。
#pragma once

namespace DockLimits {
    // ── §1.3.2 弹簧可达集上下界 ────────────────────────────────────────
    inline constexpr float kScaleOvershoot   = 1.10f;  // σ_max = 1 + 1.10·(maxScale−1)
    inline constexpr float kScaleUndershoot  = 0.10f;  // σ_min = 1 − 0.10·(maxScale−1)
    inline constexpr float kBounceOvershoot  = 1.20f;  // β_max = +1.20·A
    inline constexpr float kBounceUndershoot = 0.20f;  // β_min = −0.20·A

    // ── §1.3.5 tooltip 储备 ───────────────────────────────────────────
    // kTooltipReservePx：tooltip 在【内侧】占用 = gap(6) + slide(8) + tipH(26)
    // kTooltipMaxW     ：tooltip 最大宽度 = kMaxTipW(240) + 左右 padding(16)
    inline constexpr float kTooltipReservePx = 40.0f;
    inline constexpr float kTooltipMaxW      = 256.0f;
}
