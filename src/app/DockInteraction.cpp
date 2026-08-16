// src/app/DockInteraction.cpp
// DockEngine 子模块：窗口消息 / 鼠标 / 点击 / 命中测试 / 拖放 / 右键菜单 / 托盘 / 诊断
// 方法体从原 DockEngine.cpp 拆分，零行为变更；类声明见 DockInteraction.h
#include "DockEngine.h"
#include "DockInteraction.h"
#include "DockEngineInternal.h"
#include "IconProvider.h"    // 托盘图标：运行时从 PNG 解码（LoadTrayIcon）
#include "../utils/PathUtil.h"
#include <shlobj.h>
#include <ole2.h>
#include <commdlg.h>
#include <cmath>

void DockInteraction::AddTrayIcon() {
    if (m_owner->m_trayAdded) return;
    HWND hwnd = m_owner->GetHwnd();
    if (!hwnd) return;

    m_owner->m_nid = {};
    m_owner->m_nid.cbSize = sizeof(m_owner->m_nid);
    m_owner->m_nid.hWnd = hwnd;
    m_owner->m_nid.uID = 1;
    m_owner->m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_owner->m_nid.uCallbackMessage = WM_APP_TRAY;
    // 托盘图标：运行时从 res/icons/tray_icon.png 解码；失败回退系统默认图标（见 DockManager）。
    std::wstring trayPng = PathUtil::GetExeDir() + L"res/icons/tray_icon.png";
    HICON hTray = IconProvider::LoadTrayIcon(trayPng, GetSystemMetrics(SM_CXSMICON));
    m_owner->m_nid.hIcon = hTray ? hTray : LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    m_owner->m_trayIconOwned = (hTray != nullptr);
    wcscpy_s(m_owner->m_nid.szTip, L"openDock");
    if (Shell_NotifyIconW(NIM_ADD, &m_owner->m_nid)) {
        m_owner->m_trayAdded = true;
    }
}

void DockInteraction::RemoveTrayIcon() {
    if (!m_owner->m_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &m_owner->m_nid);
    if (m_owner->m_trayIconOwned && m_owner->m_nid.hIcon) {
        DestroyIcon(m_owner->m_nid.hIcon);
        m_owner->m_nid.hIcon = nullptr;
        m_owner->m_trayIconOwned = false;
    }
    m_owner->m_trayAdded = false;
}

void DockInteraction::HandleMouseMove(int screenX, int screenY) {
    m_owner->m_lastMousePos.x = screenX;
    m_owner->m_lastMousePos.y = screenY;

    if (m_owner->m_state == DockState::Hidden || m_owner->m_state == DockState::Exiting) return;

    RECT dockRect = m_owner->m_window ? m_owner->m_window->GetDockRect() : RECT{};
    RECT fullWin  = m_owner->m_window ? m_owner->m_window->GetFullWindowRect() : dockRect;
    POINT pt = { screenX, screenY };
    // 以「整窗（含放大/tooltip 留白）」为交互区：放大图标与 tooltip 会溢出到留白；
    // 若仅按基础 Dock 条判定（HitTestEngine 的 isInDock 用微小感应区），溢出区会被误判为
    // Dock 外，配合 WM_NCHITTEST 的 HTTRANSPARENT 导致窗口收不到鼠标消息 → 悬停放大卡死（#5/#7）。
    bool inWin = (PtInRect(&fullWin, pt) != FALSE);

    if (m_owner->m_currentLayouts.empty()) {
        m_owner->m_currentLayouts = m_owner->EnsureRestLayout();   // P0-1/2：零弹簧静息布局兜底
    }
    // D3 收口（ADR §1.5.3 #1）：这里以前漏传 senseExpandPx（默认 0），而 WM_NCHITTEST
    // 那侧传 20 —— 两个集合永不相等，中间夹一圈「系统说能点、程序说没命中」的死区。
    // 现在与 DockEngine::HitTestAt 传同一个常量，hover 判定与命中判定恒等。
    HitTestEngine::HitResult hit = m_owner->m_hitTest->Test(
        pt, dockRect, m_owner->m_currentLayouts, m_owner->m_appConfig.dock.position,
        m_owner->m_dockWidth, m_owner->m_dockHeight,
        m_owner->m_appConfig.dock.baseIconSize, m_owner->m_appConfig.dock.dockPadding,
        (float)DockConstants::SENSE_AREA_EXPAND_PX);
    m_owner->m_mouseInDock = inWin;

    if (inWin) {
        // 光标在整窗（含放大/tooltip 留白）内：无论是否命中图标，都先计算主轴位置并
        // 更新鱼眼放大目标 —— 这样当光标划过图标间隙或侧边留白时，鱼眼放大会平滑跟随
        // 光标，而非冻结在上一命中图标（>5 图标时条更长、间隙更多，不调用 ApplyHoverTargets
        // 会让放大明显「卡住」，即 #5 残留问题）。
        float mouseMain = m_owner->m_geom->screenToMainAxis(
            (float)(pt.x - dockRect.left), (float)(pt.y - dockRect.top),
            m_owner->m_dockWidth, m_owner->m_dockHeight);
        // #N：仅当该边启用鱼眼时才放大；否则保持静息（scale=1），仅做命中/悬停态更新。
        if (m_owner->IsFisheyeEnabled()) {
            m_owner->ApplyHoverTargets(mouseMain);
        } else {
            m_owner->ApplyRestTargets();
        }

        if (hit.hoveredIndex >= 0) {
            // 命中某个图标：记录悬停目标（放大图标溢出区亦能命中，halfSize 含 scale）
            m_owner->m_hoveredIndex = hit.hoveredIndex;
            if (m_owner->m_state == DockState::Idle || m_owner->m_state == DockState::Hovering) {
                m_owner->EnterState(DockState::Hovering);
            }
            m_owner->m_hideCountdown = 0.0f;
            m_owner->SetPenetration(false);
            m_owner->StartAnimationLoop();
        } else {
            // 在窗口内但不在图标上（如 tooltip 区 / 条上空隙）：保持交互态（穿透关闭），
            // 鼠标在留白内移动时鱼眼已随光标平滑跟随；保持当前 m_hoveredIndex 不变，
            // 让 tooltip 仍对应上次命中的图标，避免 tooltip 闪烁（#5/#7）。
            m_owner->SetPenetration(false);
            m_owner->m_hideCountdown = 0.0f;
        }
    } else {
        m_owner->HandleMouseLeave();
    }

    // Step 8 / #4：拖拽进行中（已越 4px 阈值）→ 实时重排预览：其它图标按光标位置即时位移，
    // 被拖图标继续跟随光标；最终顺序待 WM_LBUTTONUP 落盘。光标移出显示区由删除分支处理。
    if (m_owner->m_dragging && m_owner->m_dragMoved) {
        m_owner->LiveDragReorder(pt);
    }

    // #1：若本次移动使鱼眼进入放大态，启动看门狗以探测光标离开（即便非穿透/非自动隐藏），
    // 防止 WM_MOUSELEAVE 因 HTTRANSPARENT 穿透漏发导致放大卡死。
    m_owner->UpdateIdleWatchdog();
}

void DockInteraction::HandleMouseLeave() {
    m_owner->m_mouseInDock  = false;
    m_owner->m_hoveredIndex = -1;
    // #1 修复：无论当前状态（含 Idle）都必须无条件复位鱼眼放大（scale 目标→1.0）。
    // 上一轮 #5 修复让 inWin 时「始终」ApplyHoverTargets（含间隙/侧边留白），此时
    // state 仍为 Idle；若光标在留白内离开窗口（从未 hover 到图标），旧逻辑只在
    // Hovering/Bouncing 才复位 → 放大目标不回弹，弹簧收敛在放大值 → 卡死。
    // 右侧竖条最易触发（左侧大片交互留白）。改为无条件复位。
    m_owner->ApplyRestTargets();
    // 鼠标离开：进入空闲点击穿透态（Step 7），不遮挡下方窗口。须在 ApplyRestTargets
    // 之后调用，使其内部 UpdateIdleWatchdog 因 scale 已复位而停止看门狗（非自动隐藏/
    // 穿透态下无需持续轮询）。
    m_owner->SetPenetration(true);
    m_owner->StartAnimationLoop();   // 动画回落，收敛后状态机迁移到 IDLE

    // Bugfix（用户报障：「鼠标离开图标但未离开感应区时，图标缩到最小」）：
    // 感应带（reveal）沿【主轴】横跨整个工作区（Top 例：x∈[0,2560]），而整窗 fullWin
    // 只有「条宽 + 左右留白」（例：x∈[1164,1396]）—— 二者只在【法向】上是包含关系，
    // 主轴上 reveal 远宽于 fullWin。因此光标贴着屏幕边缘【侧向】滑出 Dock 条时，
    // 已离开窗口（真实 GUI 触发 WM_MOUSELEAVE → 本函数），却【仍在本边感应带内】。
    // 原实现在此处无条件进入自动隐藏（hideDelayMs 默认 0 → 立即 Hide()），而
    // Hide() → ApplyExitTargets() 把所有图标 scale 目标打到 0.0，视觉上正是用户描述的
    // 「缩到最小」；随后看门狗又因光标仍在带内 Show() 回来，形成「缩没→重新弹出」抖动。
    // 正解：仍在本边感应带内 → 只把鱼眼复位到正常大小（上方 ApplyRestTargets 已完成），
    // 【不】启动隐藏；待光标真正离开感应带后，由 TickIdle 的兜底分支（!inFull 且
    // !inOwnReveal）统一收起，语义与看门狗完全一致，不新增第二套感应区定义。
    bool inOwnReveal = false;
    if (m_owner->m_window) {
        POINT cur = {};
        if (!GetCursorPos(&cur)) cur = m_owner->m_lastMousePos;   // 取不到真实光标时退回最后已知位置
        RECT dr = m_owner->m_window->GetDockRect();
        RECT ownReveal = m_owner->ComputeRevealZoneFor(m_owner->m_appConfig.dock.position, dr);
        inOwnReveal = m_owner->IsEdgeEnabled(m_owner->m_appConfig.dock.position)
                      && (PtInRect(&ownReveal, cur) != 0);
    }

    // 自动隐藏：离开后启动隐藏延迟倒计时（仅当可见、未处于隐藏流程、且已真正离开本边感应带）
    if (m_owner->m_autoHide && !inOwnReveal && m_owner->m_state != DockState::Hidden &&
        m_owner->m_state != DockState::Exiting && m_owner->m_hideCountdown <= 0.0f) {
        if (m_owner->m_hideDelayMs <= 0) {
            m_owner->Hide();   // 零延迟：离开感应区立即隐藏（需求3/4）
        } else {
            m_owner->m_hideCountdown = m_owner->m_hideDelayMs / 1000.0f;
        }
    }
}

// BUG3：动作路径的唯一命中真源 —— 就地、无状态、按下点重算。
// 与 HandleMouseMove / HitTestAt 共用同一个 Test + 同一个 SENSE_AREA_EXPAND_PX，
// 因此「系统说能点」「视觉说悬停」「动作说命中」三者用的是同一套几何，不会再分叉。
int DockInteraction::ResolveHitIndexAt(POINT pt) const {
    if (!m_owner->m_hitTest) return -1;
    // 隐藏/退出态不接受任何动作命中（与 HitTestAt 的可见性判据保持一致）。
    if (m_owner->m_state == DockState::Hidden || m_owner->m_state == DockState::Exiting) return -1;

    if (m_owner->m_currentLayouts.empty()) {
        m_owner->m_currentLayouts = m_owner->EnsureRestLayout();
    }
    RECT dockRect = m_owner->m_window ? m_owner->m_window->GetDockRect() : RECT{};
    HitTestEngine::HitResult hit = m_owner->m_hitTest->Test(
        pt, dockRect, m_owner->m_currentLayouts, m_owner->m_appConfig.dock.position,
        m_owner->m_dockWidth, m_owner->m_dockHeight,
        m_owner->m_appConfig.dock.baseIconSize, m_owner->m_appConfig.dock.dockPadding,
        (float)DockConstants::SENSE_AREA_EXPAND_PX);

    // 越界即视为未命中：布局数组与配置数组可能在增删图标后短暂不同步，
    // 下游 RemoveIcon/LaunchIcon 直接吃下标，必须在此拦住。
    const int idx = hit.hoveredIndex;
    if (idx < 0 || idx >= m_owner->GetIconCount()) return -1;
    return idx;
}

void DockInteraction::HandleClick(int screenX, int screenY) {
    POINT pt = { screenX, screenY };
    m_owner->HandleMouseMove(screenX, screenY);
    // BUG3：这里【不能】用 m_hoveredIndex。它是粘滞的视觉态（MISS 时刻意不清零以稳住
    // tooltip），在按下点其实没命中时会残留上一帧的旧下标 —— 表现为「图标看得见、
    // 点下去却弹了另一张/启动了错的应用」。改为就地重算按下点的命中。
    const int hitIndex = ResolveHitIndexAt(pt);
    if (hitIndex >= 0 &&
        (m_owner->m_state == DockState::Hovering || m_owner->m_state == DockState::Idle
         || m_owner->m_state == DockState::Bouncing)) {
        float cx = 0.0f, cy = 0.0f;
        m_owner->GetIconCurrentScreenCenter(hitIndex, cx, cy);
        // 命中即把视觉悬停态对齐到动作命中，避免 tooltip 指向另一张图标。
        m_owner->m_hoveredIndex = hitIndex;
        m_owner->TriggerBounce(hitIndex);
        m_owner->EnterState(DockState::Bouncing);
    }
}

// ═══════════════════════════════════════════════════════════
// 消息处理（Windowed）
// ═══════════════════════════════════════════════════════════

LRESULT DockInteraction::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hwnd, &pt);
        m_owner->HandleMouseMove(pt.x, pt.y);
        // Step 8：拖拽排序进行中 — 检测位移是否超过阈值（4px）以区分点击/拖拽
        if (m_owner->m_dragging) {
            int dx = pt.x - m_owner->m_dragStart.x;
            int dy = pt.y - m_owner->m_dragStart.y;
            if (dx * dx + dy * dy > 4 * 4) m_owner->m_dragMoved = true;
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
        m_owner->HandleMouseLeave();
        break;

    // ── 显示桌面 / Win+D / Win+M 多层防御（第一层：WM_SYSCOMMAND 拦截）──
    // dock 是 WS_EX_TOOLWINDOW 无任务栏按钮，一旦被最小化无法手工恢复，只能重启进程。
    // 凡经 WM_SYSCOMMAND 发起的最小化（部分任务管理器 / 自动化工具 / Alt+Space 路径）直接吞掉，
    // 不转发 DefWindowProc，窗口永不进入最小化态。业务隐藏（autoHide/Hidden）走 SW_HIDE，
    // 不经过 WM_SYSCOMMAND，故此处拦截不影响 autoHide。
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE) return 0;
        break;

    // ── 第二层：WM_SIZE(SIZE_MINIMIZED) 兜底（覆盖 Explorer「显示桌面」直接
    //    ShowWindow(SW_MINIMIZE) 的路径，该路径不经由 WM_SYSCOMMAND）──
    // 仅当业务态仍要求可见（m_window->IsVisible()==true）才恢复；autoHide 隐藏态
    // （IsVisible()==false）放行，绝不误唤出。恢复在内部完成，此处吸收该通知。
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            if (m_owner->m_window && m_owner->m_window->IsVisible())
                m_owner->RestoreFromOsMinimize();
            return 0;
        }
        break;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hwnd, &pt);
        m_owner->HandleClick(pt.x, pt.y);              // 命中图标 → 弹跳动画（视觉反馈）
        // Step 8：记录拖拽起点；真实启动延后到 WM_LBUTTONUP（未移动=点击启动，移动=重排）
        //
        // BUG3：拖拽下标必须来自【按下点的即时命中】，不能沿用粘滞的 m_hoveredIndex。
        // 旧写法 `if (m_hoveredIndex >= 0) m_dragIndex = m_hoveredIndex;` 会在按下点
        // 未命中时把上一帧的旧下标塞进 m_dragIndex，随后 WM_LBUTTONUP / TickIdle 的
        // 拖拽删除分支就会 RemoveIcon 掉一张用户压根没按到的图标（真机 4→3→2）。
        const int downIndex = ResolveHitIndexAt(pt);
        if (downIndex >= 0) {
            m_owner->m_dragIndex  = downIndex;
            m_owner->m_dragStart  = pt;
            m_owner->m_dragging   = true;
            m_owner->m_dragMoved  = false;
            SetCapture(hwnd);                  // 捕获鼠标，拖出窗口仍收得到 MOVE/UP
        } else {
            // 按下点未命中任何图标 → 彻底清空拖拽态，杜绝残留下标被后续分支消费。
            m_owner->m_dragging  = false;
            m_owner->m_dragMoved = false;
            m_owner->m_dragIndex = -1;
        }
        break;
    }
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hwnd, &pt);
        // BUG3 删除守卫：m_dragIndex 只有在【按下点确实命中过一张图标】时才 >= 0
        //（见 WM_LBUTTONDOWN）。这里再做一次范围校验，因为拖拽期间可能有增删图标
        // 使下标失效。任何 -1 / 越界一律不删、不重排 —— 宁可丢一次手势，也绝不误删。
        const bool dragIndexValid =
            (m_owner->m_dragIndex >= 0 && m_owner->m_dragIndex < m_owner->GetIconCount());
        if (m_owner->m_dragging) {
            // Bugfix（用户报障 B：「点击启动后仍被误判为拖拽 → 离开感应区即删除」）：
            // 【先收状态、再执行动作】。手势在左键抬起这一刻就已经结束，状态必须立即落地，
            // 动作只是它的后果。旧写法把动作放在前面、状态清理放在后面，于是整个动作执行
            // 期间 m_dragging 仍为 true 且窗口仍持有 SetCapture —— 而 LaunchIcon →
            // ShellExecuteW 是同步阻塞且【会泵消息】的（DDE / Shell 会话）：泵出来的
            // WM_MOUSEMOVE 会把 m_dragMoved 置真，泵出来的 WM_APP_IDLE 会走 TickIdle 的
            // 拖拽删除分支，用户「启动完应用把鼠标挪开」就变成了「拖出感应区 → 删除图标」。
            // 快照 + 先清理让本函数对任何重入都免疫：重入时看到的已经是干净的非拖拽态。
            const int  actIndex = m_owner->m_dragIndex;
            const bool actMoved = m_owner->m_dragMoved;
            m_owner->m_dragging   = false;
            m_owner->m_dragMoved  = false;
            m_owner->m_dragIndex  = -1;
            ReleaseCapture();

            if (!dragIndexValid) {
                // 下标失效：不删、不重排（宁可丢一次手势，也绝不误删）
            } else if (actMoved) {
                RECT dr = m_owner->m_window ? m_owner->m_window->GetDockRect() : RECT{};
                RECT dropR = dr;
                InflateRect(&dropR, 8, 8);   // 容差：拖出 Dock 条外即视为「拖拽删除」
                if (PtInRect(&dropR, pt)) {
                    int insert = m_owner->ComputeDragInsertIndex(pt);   // 在 Dock 内释放 → 重排
                    // 实时重排期间数据已按光标更新、但 persist=false 未落盘；松开时若最终位置
                    // 已就位（ReorderIcon 返回 false 不落盘），需显式 PersistConfig 把最终顺序
                    // 写入磁盘，否则实时重排结果会丢失。
                    if (!m_owner->ReorderIcon(actIndex, insert, true)) {
                        m_owner->PersistConfig();
                    }
                } else {
                    m_owner->RemoveIcon(actIndex, true);             // 拖出 Dock → 删除
                }
            } else {
                m_owner->LaunchIcon(actIndex);                       // 纯点击（未移动）→ 启动应用
            }
        }
        break;
    }
    case WM_CAPTURECHANGED:
        // Bugfix（用户报障 B 的第二条泄漏路径）：捕获被【外部】夺走时必须撤销拖拽手势。
        // 点击启动后被拉起的应用会 SetForegroundWindow，系统据此把鼠标捕获从本窗口收走
        // 并投递本消息，而 WM_LBUTTONUP 可能已经不会再到达本窗口 —— 此时若放任
        // m_dragging/m_dragMoved 留真，TickIdle 的拖拽删除分支就会在鼠标离开感应区时删图标。
        // 语义上「没有捕获 = 没有进行中的拖拽」，故一律清空，不执行任何删除/重排/启动。
        // 我们自己调用 ReleaseCapture() 也会同步收到本消息，但那时状态已清空，此处幂等无副作用。
        m_owner->m_dragging  = false;
        m_owner->m_dragMoved = false;
        m_owner->m_dragIndex = -1;
        break;
    // 需求：去掉四个边的右键菜单。Dock 栏不再响应右键（不弹菜单）；点击/命中逻辑不受影响。
    case WM_APP_TICK: {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - m_owner->m_lastTickTime.QuadPart)
                 / (float)m_owner->m_perfFrequency.QuadPart;
        m_owner->m_lastTickTime = now;
        m_owner->OnAnimationTick(dt);
        break;
    }
    case WM_APP_IDLE: {   // Step 7：看门狗空闲探测（仅窗口化构建）
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - m_owner->m_lastIdleTime.QuadPart)
                 / (float)m_owner->m_perfFrequency.QuadPart;
        m_owner->m_lastIdleTime = now;
        m_owner->TickIdle(dt > 0.0f ? dt : 0.1f);
        break;
    }
    case WM_DPICHANGED:        // 每显示器 DPI 变化（跨屏拖动/缩放设置变更）
    case WM_DISPLAYCHANGE: {   // 分辨率/显示器拓扑变化
        if (m_owner->m_render && m_owner->m_render->GetMode() == RenderManager::Mode::Windowed
            && m_owner->m_window) {
            m_owner->ApplyPlacement();   // Step 10：按配置的停靠边/偏移/显示器统一定位
        }
        break;
    }
    case WM_NCHITTEST: {
        // 选择性命中测试（取代全局 WS_EX_TRANSPARENT 穿透）：
        //   Dock 条矩形内 → HTCLIENT：始终可交互（右键菜单 / 拖拽添加 / 点击启动）
        //   可见态（dock 非 Hidden/Exiting）→ 整窗 fullWin（含放大/tooltip 留白）均返回 HTCLIENT，避免放大图标溢出留白区
        //     变 HTTRANSPARENT 后窗口收不到鼠标消息 → 悬停放大卡死（#5/#7）；
        //   其余（空闲态的透明留白）→ HTTRANSPARENT：穿透，不遮挡桌面与其它窗口。
        // 命中逻辑统一收敛到 HitTestAt，便于无头探针与真实路径共用同一套几何。
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        return m_owner->HitTestAt(pt.x, pt.y) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_DESTROY:
        // 注意：此处不再 PostQuitMessage。消息循环由 DockManager 统一拥有（多 Dock 共享同一
        // 线程循环），任何单个 Dock 窗口销毁（含切换 DockManager 前对单实例 engine 的
        // Shutdown、或运行时关闭某条边）都不应终止整个应用；退出仅由托盘「退出」菜单显式
        // PostQuitMessage 发起。否则切换 DockManager 时单实例 engine 的 DestroyWindow 会误
        // 入队 WM_QUIT，使 DockManager::Run 的消息循环在第一帧就退出 → 双击无反应、进程秒退。
        break;

    case WM_APP_TRAY: {
        // 托盘图标回调：左/右键弹出菜单，提供退出入口
        UINT tm = (UINT)lParam;
        if (tm == WM_RBUTTONUP || tm == WM_LBUTTONUP || tm == WM_CONTEXTMENU) {
            POINT pt = {};
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            if (hMenu) {
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出 openDock");
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(
                    hMenu,
                    TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                    pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                if (cmd == (int)ID_TRAY_EXIT) PostQuitMessage(0);
                PostMessageW(hwnd, WM_NULL, 0, 0);
            }
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT) PostQuitMessage(0);
        else m_owner->HandleMenuCommand((int)LOWORD(wParam));   // Step 13：Dock 菜单命令
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

void DockInteraction::HandleMenuCommand(int cmd) {
    switch (cmd) {
    case DockEngine::ID_DOCK_ADD:
        m_owner->AddAppViaDialog();
        break;
    case DockEngine::ID_DOCK_ADD_FOLDER:   // #2 添加文件夹
        m_owner->AddFolderViaDialog();
        break;
    // #5：图层位置（Z 序）
    case DockEngine::ID_ZORDER_FRONT:  m_owner->SetZOrder(1);  break;
    case DockEngine::ID_ZORDER_NORMAL: m_owner->SetZOrder(0);  break;
    case DockEngine::ID_ZORDER_BACK:   m_owner->SetZOrder(-1); break;
    case DockEngine::ID_DOCK_REMOVE: {
        POINT p; GetCursorPos(&p);
        if (m_owner->m_currentLayouts.empty())
            m_owner->m_layout->CalculateLayout(m_owner->m_appConfig.dock, 0.0f, false,
                                               *m_owner->m_springs, m_owner->m_currentLayouts);
        HitTestEngine::HitResult h = m_owner->m_hitTest->Test(
            p, m_owner->m_window ? m_owner->m_window->GetDockRect() : RECT{},
            m_owner->m_currentLayouts,
            m_owner->m_appConfig.dock.position, m_owner->m_dockWidth, m_owner->m_dockHeight,
            m_owner->m_appConfig.dock.baseIconSize, m_owner->m_appConfig.dock.dockPadding,
            (float)DockConstants::SENSE_AREA_EXPAND_PX);
        if (h.hoveredIndex >= 0) m_owner->RemoveIcon(h.hoveredIndex, true);
        break;
    }
    // #3：位置菜单项 = 四边吸附独立开关（勾选切换启用/禁用），而非单选
    case DockEngine::ID_POS_TOP:    m_owner->SetEdgeEnabled(DockPosition::Top,    !m_owner->IsEdgeEnabled(DockPosition::Top));    break;
    case DockEngine::ID_POS_BOTTOM: m_owner->SetEdgeEnabled(DockPosition::Bottom, !m_owner->IsEdgeEnabled(DockPosition::Bottom)); break;
    case DockEngine::ID_POS_LEFT:   m_owner->SetEdgeEnabled(DockPosition::Left,   !m_owner->IsEdgeEnabled(DockPosition::Left));   break;
    case DockEngine::ID_POS_RIGHT:  m_owner->SetEdgeEnabled(DockPosition::Right,  !m_owner->IsEdgeEnabled(DockPosition::Right));  break;
    case DockEngine::ID_SIZE_S: m_owner->SetIconSize(DockEngine::SIZE_SMALL); break;
    case DockEngine::ID_SIZE_M: m_owner->SetIconSize(DockEngine::SIZE_MED);   break;
    case DockEngine::ID_SIZE_L: m_owner->SetIconSize(DockEngine::SIZE_LARGE); break;
    case DockEngine::ID_OPACITY_25:  m_owner->SetBackgroundOpacity(0.25f); break;
    case DockEngine::ID_OPACITY_50:  m_owner->SetBackgroundOpacity(0.50f); break;
    case DockEngine::ID_OPACITY_75:  m_owner->SetBackgroundOpacity(0.75f); break;
    case DockEngine::ID_OPACITY_100: m_owner->SetBackgroundOpacity(1.00f); break;
    case DockEngine::ID_BAR_TOGGLE:  m_owner->SetDockBarVisible(!m_owner->m_appConfig.dockBarVisible); break;  // #N 底座显隐
    case ID_TRAY_EXIT: PostQuitMessage(0); break;
    }
}
