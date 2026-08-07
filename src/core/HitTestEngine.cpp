// src/core/HitTestEngine.cpp
#include "HitTestEngine.h"
#include <cmath>

HitTestEngine::HitResult HitTestEngine::Test(
    POINT screenPos,
    const RECT& dockRect,
    const std::vector<IconLayout>& layouts,
    DockPosition pos,
    float dockWidth,
    float dockHeight,
    float baseIconSize,
    float dockPadding,
    float senseExpandPx) const
{
    HitResult result;

    // 统一几何（自愈）：首次或停靠边变化时任一重建，保证始终用正确边的几何，
    // 不依赖任何调用方主动同步当前边（消除"忘调用同步"类 bug）。
    if (!m_geom || m_lastPos != pos) {
        m_geom = MakeGeometry(pos);
        m_lastPos = pos;
    }

    // 1. 判断是否在 Dock 区域内（含扩展感应区，水平/竖直均双向扩展）。
    //    注意：isInDock 仅作信息字段，不再作为硬过滤——下方逐图标命中自带图标矩形
    //    （含 SENSE 膨胀）判定，光标在图标附近即返回正确 hoveredIndex。放宽后修复
    //    Bottom/Right（RestAtFarEdge=true）因 m_baseRect 与渲染对齐偏差导致的漏判。
    //
    //    D3 收口（实现期新发现，ADR §1.5 未列出）：这里以前【写死】
    //    DockConstants::SENSE_AREA_EXPAND_PX，与形参 senseExpandPx 各走各的 ——
    //    同一个函数内部就存在两个互不相干的膨胀量，调用方传 0 时条膨胀仍是 20。
    //    这正是「命中域四份不一致」的最内层一份。现统一由 senseExpandPx 驱动。
    const int expandI = (int)std::lround(senseExpandPx);
    RECT senseRect = dockRect;
    InflateRect(&senseRect, expandI, expandI);
    result.isInDock = (PtInRect(&senseRect, screenPos) != FALSE);

    // 2. 转换为 Dock 局部坐标
    float localX = static_cast<float>(screenPos.x - dockRect.left);
    float localY = static_cast<float>(screenPos.y - dockRect.top);
    result.mouseMainInDock = m_geom->screenToMainAxis(localX, localY, dockWidth, dockHeight);

    // 3. 逐图标命中检测 —— ADR §1.5 单一真源：命中矩形只能来自 iconHitRect
    //    （= iconVisualRect ⊕ senseExpandPx）。这里不再自行 mapLayout+half，
    //    杜绝「渲染矩形」与「命中矩形」二次分叉（D3）。
    //
    //    选取规则（实现期新发现的第四个不对称源，ADR §1.2 未覆盖）：
    //    旧代码是「升序首个包含即 break」。相邻图标的 hitRect 因 ⊕s 必然交叠
    //    （spacing 8~10 px ≪ 2s = 40 px），交叠区永远判给【小下标】那个 ——
    //    这是一个沿主轴的【单向偏置】，与"中心对称"（需求 5）直接矛盾，
    //    也与渲染相反：RenderManager 按数组顺序绘制，压在最上面的是【大下标】，
    //    于是"看得见的那张图标"和"点中的那张图标"可以不是同一张。
    //    用户表现即"必须点图标的某一侧才点得中"（需求 1/3）。
    //
    //    改为两趟、与下标顺序无关、与绘制层级一致的判据：
    //      A) 先在【可见矩形】命中集合里选 —— 保证"看得见的图标像素一定命中
    //         它自己"，光晕永远抢不走实体像素；
    //      B) 可见集合为空时（光标落在间隙/留白），再在【膨胀矩形】里选。
    //    每趟内部的优先级：scale 大者优先（= LayoutEngine 的 zIndex 语义，
    //    放大中的图标就是前景层）→ 中心距近者优先 → 下标小者优先。
    //    三级判据全部关于图标中心对称，四条边共用同一套逻辑。
    const IconGeometryParams gp{ dockWidth, dockHeight, baseIconSize, dockPadding };

    // BUG3 收口（MISS 语义单一真源）：kNoHit 必须是【唯一】的“未命中”哨兵值，且
    // 必须 < 0。历史缺陷：Cand 的默认 idx 若为 0，则一次真正的 MISS 会产出
    // hoveredIndex = 0 —— 与「命中 0 号图标」完全无法区分。真机日志里
    // 「[HIT] MISS ... hovered=0」正是这个形态：判定说没命中，索引却指向一个真实
    // 图标，下游 RemoveIcon(0) 于是误删用户第一个图标（观察到 4→3→2）。
    // 用具名常量 + 编译期断言把这条不变式钉死，任何人改默认值都会编译失败。
    constexpr int kNoHit = -1;
    static_assert(kNoHit < 0, "kNoHit 必须为负：hoveredIndex>=0 是下游判定命中的唯一依据");

    struct Cand { int idx = kNoHit; float scale = 0.0f; float d2 = 0.0f; };
    auto better = [](const Cand& a, const Cand& b) {   // a 是否优于 b
        if (b.idx < 0) return true;
        if (a.scale > b.scale + 1e-4f) return true;
        if (a.scale < b.scale - 1e-4f) return false;
        return a.d2 < b.d2;
    };
    Cand bestVis, bestHalo;
    // 防御：确认两个累加器起始即为 MISS（而非 0 号图标）。
    if (bestVis.idx >= 0 || bestHalo.idx >= 0) { bestVis.idx = kNoHit; bestHalo.idx = kNoHit; }
    for (int i = 0; i < static_cast<int>(layouts.size()); ++i) {
        const IconLayout& L = layouts[static_cast<size_t>(i)];
        const RectF vis = m_geom->iconVisualRect(L, gp);
        const float dx = localX - vis.cx(), dy = localY - vis.cy();
        const Cand c{ i, L.scale, dx * dx + dy * dy };
        if (vis.contains(localX, localY)) {
            if (better(c, bestVis)) bestVis = c;
        } else if (bestVis.idx < 0 && vis.inflated(senseExpandPx).contains(localX, localY)) {
            if (better(c, bestHalo)) bestHalo = c;
        }
    }
    // 归一化：两趟都没选出候选 → 必须是 kNoHit，绝不能退化成任何合法下标。
    const int picked = (bestVis.idx >= 0) ? bestVis.idx : bestHalo.idx;
    result.hoveredIndex =
        (picked >= 0 && picked < static_cast<int>(layouts.size())) ? picked : kNoHit;

    return result;
}

bool HitTestEngine::IsInCornerIdleZone(POINT pt, RECT workArea, int cornerPx) {
    if (cornerPx <= 0) return false;
    const int L = workArea.left, T = workArea.top;
    const int R = workArea.right, B = workArea.bottom;
    // 四个角各取边长 cornerPx 的正方形（相邻两带法向厚度交叠正方形，取较小带厚
    // cornerPx = min(dockWidth, dockHeight) 保证角格完全落在两带交叠内）。
    auto inSquare = [&](int x0, int y0) {
        RECT sq = { x0, y0, x0 + cornerPx, y0 + cornerPx };
        return PtInRect(&sq, pt) != FALSE;
    };
    return inSquare(L, T)
        || inSquare(R - cornerPx, T)
        || inSquare(L, B - cornerPx)
        || inSquare(R - cornerPx, B - cornerPx);
}
