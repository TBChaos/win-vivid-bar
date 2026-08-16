// src/app/DockStateMachine.cpp
// DockEngine 子模块：动画状态机 / 自动隐藏看门狗 / 边切换
// 方法体从原 DockEngine.cpp 拆分，零行为变更；类声明见 DockStateMachine.h
#include "DockEngine.h"
#include "DockStateMachine.h"
#include "DockEngineInternal.h"
#include "../utils/PathUtil.h"
#include <shlobj.h>
#include <ole2.h>
#include <commdlg.h>
#include <cmath>   // std::isfinite

void DockStateMachine::EnterState(DockState next) {
    if (m_owner->m_state == next) return;
    m_owner->m_state = next;
    m_owner->m_stateTime = 0.0f;
}

void DockStateMachine::UpdateStateMachine(bool allSettled) {
    switch (m_owner->m_state) {
    case DockState::Entering:
        // 全部图标已释放且弹簧收敛 → IDLE
        if (m_owner->m_entryReleased >= m_owner->m_appConfig.dock.iconCount && allSettled) {
            m_owner->EnterState(DockState::Idle);
            m_owner->StopAnimationLoop();
        }
        break;

    case DockState::Bouncing:
        // 弹跳回落完成 → 悬停或静息
        if (m_owner->m_bounceResetTimer < 0.0f && allSettled) {
            m_owner->EnterState(m_owner->m_mouseInDock ? DockState::Hovering : DockState::Idle);
            if (m_owner->m_state == DockState::Idle) m_owner->StopAnimationLoop();
        }
        break;

    case DockState::Hovering:
        if (!m_owner->m_mouseInDock && allSettled) {
            m_owner->EnterState(DockState::Idle);
            m_owner->StopAnimationLoop();
        }
        break;

    case DockState::Exiting:
        if (allSettled) {
            m_owner->EnterState(DockState::Hidden);
            m_owner->m_window->Show(false);
            // 隐藏后处于点击穿透态（不遮挡其它窗口），保持看门狗以探测边缘唤出
            m_owner->SetPenetration(true);
            m_owner->StopAnimationLoop();
        }
        break;

    case DockState::Idle:
        if (allSettled) m_owner->StopAnimationLoop();   // 空闲静默：CPU 0%
        break;

    default:
        break;
    }
}

// ═══════════════════════════════════════════════════════════
// 自动隐藏 + 空闲鼠标穿透（Step 7）
// ═══════════════════════════════════════════════════════════

void DockStateMachine::SetPenetration(bool penetrate) {
    // P0-5：不再调用 WindowManager::SetMousePenetration（该公共 API 已删除）。
    // 穿透语义完全由 WM_NCHITTEST 的逐区域判定承载（Dock 条→HTCLIENT，
    // 透明留白→HTTRANSPARENT），窗口从不直接挂 WS_EX_TRANSPARENT。
    // 此处 m_mousePenetrating 仅作为看门狗状态标志：穿透态需看门狗持续探测
    // 光标重新进入以恢复交互；与窗口样式无关。
    if (m_owner->m_mousePenetrating == penetrate) return;
    m_owner->m_mousePenetrating = penetrate;
    m_owner->UpdateIdleWatchdog();   // 穿透态需要看门狗探测光标重新进入
}

void DockStateMachine::UpdateIdleWatchdog() {
    // 看门狗在以下任一情况运行：
    //  - 自动隐藏开启（隐藏/显示均需探测光标重新进入）；或
    //  - 当前处于空闲鼠标穿透态（需探测进入以恢复交互）；或
    //  - 鱼眼处于放大态（#1：即使非穿透/非自动隐藏，也需持续探测光标是否已离开
    //    窗口/感应区，以防 WM_MOUSELEAVE 因 HTTRANSPARENT 穿透被漏发而导致放大卡死）。
    //    看门狗每 100ms（~10fps）轮询光标位置，离开后复位并自停（见 TickIdle）。
    //
    // P0 遮挡挂起（核心一行）：!m_occluded 前置于【整条】判据之前，而非只挂 autoHide 分支。
    // 原因（已复核）：非 autoHide 常显模式下 HandleMouseLeave（DockInteraction.cpp:126）
    // 无条件调 SetPenetration(true) → m_mousePenetrating=true → 看门狗照样常驻。
    // 只挂 autoHide 分支会漏掉常显模式这条常驻路径，遮挡态 CPU 仍归不了零。
    if (!m_owner->m_occluded &&
        (m_owner->m_autoHide || m_owner->m_mousePenetrating || m_owner->AnyScaleElevated()))
        m_owner->StartWatchdog();
    else
        m_owner->StopWatchdog();
}

// P0 遮挡挂起：由编排层 DockManager::RecomputeOcclusion（事件驱动 + 兜底定时器）调用。
// P0 的挂起动作刻意最小：只停「时间驱动源」（动画循环 + 看门狗），不动窗口可见性。
//
// P1-6 在此之上追加【释放 DComp 合成资源】：把仍然可见的边 Show(false)，让 DWM
// 释放该窗口的合成表面。适用语境必须说清楚：
//   - autoHide 场景下 dock 本就处于 SW_HIDE（Exiting→Hidden 已 m_window->Show(false)），
//     没有合成资源可释放，此处只做挂起、【绝不】碰窗口可见性；
//   - P1-6 真正服务的是【非 autoHide 常显态被遮挡】：窗口一直挂在那儿，DWM 仍为它
//     保留合成表面，只有 Show(false) 才能把这部分吐出去。
// 恢复的对称性由 m_occlusionHidWindow 保证：只有「是我们主动 hide 的」才 Show(true)，
// autoHide 的隐藏态一律交还给 autoHide 自身的 reveal 逻辑。
// 【需真机目检】Show(true) 之后的首帧恢复延迟 / 是否有一次可察觉的闪烁 —— 沙盒无法评估。
void DockStateMachine::SetOccluded(bool on) {
    if (m_owner->m_occluded == on) return;   // 幂等：兜底定时器每秒重检，同值不做无谓工作
    m_owner->m_occluded = on;
    if (on) {
        // 遮挡：先停动画循环（被完全盖住时任何插值都是不可见的无效功），
        // 再走判据 —— 此时 !m_occluded 为假 → 必定 StopWatchdog（100ms 轮询归零）。
        m_owner->StopAnimationLoop();
        m_owner->UpdateIdleWatchdog();
        // P1-6 释放合成资源。三重守卫缺一不可：
        //   ① 状态守卫：Hidden / Exiting 不碰。Hidden 本就不可见；Exiting 正在收起，
        //      让它保持原状（上面 StopAnimationLoop 已把它冻住，解除时下方补驱动）。
        //   ② 可见性守卫：窗口已是 SW_HIDE 时不重复 hide —— 否则 m_occlusionHidWindow
        //      会被误置真，解除遮挡时把本该隐藏的 autoHide dock 直接弹出来。
        //   ③ 复用既有封装 WindowManager::Show(bool)（src/platform/WindowManager.cpp:161，
        //      内部 ShowWindow(SW_SHOWNOACTIVATE / SW_HIDE) 并维护 m_visible），
        //      不裸调 ShowWindow，否则 m_visible 与真实可见性脱钩、②的守卫立刻失效。
        const bool visibleState = (m_owner->m_state != DockState::Hidden &&
                                   m_owner->m_state != DockState::Exiting);
        if (visibleState && m_owner->m_window && m_owner->m_window->IsVisible()) {
            m_owner->m_window->Show(false);
            m_owner->m_occlusionHidWindow = true;
        }
    } else {
        // P1-6 恢复：仅当此前是【我们】隐藏的才还原。Show(true) 只是让窗口重新参与
        // 合成，DComp 表面内容此刻可能已失效/为空，故紧跟一帧 CommitFrame 把当前
        // m_currentLayouts 重新提交上去，避免先闪一下空白再等下次交互才画。
        if (m_owner->m_occlusionHidWindow) {
            m_owner->m_occlusionHidWindow = false;
            if (m_owner->m_window) m_owner->m_window->Show(true);
            if (m_owner->m_render) m_owner->m_render->CommitFrame();
        }
        // 解除：按原判据恢复。若 autoHide / 穿透 / 放大仍成立则看门狗重新起转；
        // 三者都不成立本就不该有轮询，保持停摆即为正确。
        m_owner->UpdateIdleWatchdog();
        // 动画循环仍然不在静止态主动重启（保持 P0 语义：静止态无需动画，后续任何交互
        // HandleMouseMove / Show / Hide 都会自行 StartAnimationLoop）。
        // 唯一例外：挂起时把一段【未收敛】的动画掐在了半途（典型是 Exiting 收起动画
        // 进行中被判遮挡 → StopAnimationLoop → 状态永远到不了 Hidden）。此时必须有人
        // 把它推完，否则恢复后会定格在中间帧。
        if (!m_owner->AreSpringsSettled()) m_owner->StartAnimationLoop();
    }
}

void DockStateMachine::StartWatchdog() {
    if (m_owner->m_watchdogHandle) return;
    QueryPerformanceCounter(&m_owner->m_lastIdleTime);
    // 100ms（~10fps）探测：兼顾灵敏度与 CPU 开销（点击穿透态重新捕获光标所需）
    CreateTimerQueueTimer(
        &m_owner->m_watchdogHandle, nullptr,
        [](PVOID ctx, BOOLEAN) {
            auto* engine = static_cast<DockEngine*>(ctx);
            HWND hwnd = engine->GetHwnd();
            if (hwnd) PostMessageW(hwnd, WM_APP_IDLE, 0, 0);
        },
        m_owner, 0, 100, WT_EXECUTEDEFAULT);
}

void DockStateMachine::StopWatchdog() {
    if (m_owner->m_watchdogHandle) {
        DeleteTimerQueueTimer(nullptr, m_owner->m_watchdogHandle, INVALID_HANDLE_VALUE);
        m_owner->m_watchdogHandle = nullptr;
    }
}

void DockStateMachine::TickIdle(float dt) {
    // P0 遮挡挂起守卫：被完全遮挡时本函数整体不执行。
    // 真实构建下 SetOccluded(true) 已 StopWatchdog，正常不会再进来；此守卫兜的是
    // 「StopWatchdog 之前定时器线程已 PostMessageW(WM_APP_IDLE) 入队」的竞态 ——
    // DeleteTimerQueueTimer 只等回调返回，等不了已经躺在消息队列里的那一条，
    // 它会在挂起之后被 DispatchMessage 派发一次，足以在全屏游戏上方把 dock 唤出来。
    if (m_owner->m_occluded) return;
    // 推进显示/隐藏延迟倒计时
    m_owner->AdvanceAutoHide(dt);
    // #7：自动隐藏模式下隐藏态仍需探测感应区以唤起 dock；仅非自动隐藏的隐藏态提前返回
    if (m_owner->m_state == DockState::Hidden && !m_owner->m_autoHide) return;

    // 多引擎模型：本引擎被 CreateEdgeEngine 钉死在 m_appConfig.dock.position 一条边，
    // 拥有独立窗口。因此只探测【自身所服务的那条边】，绝不跨边计算感应区，也绝不调用
    // SetDockPosition 把自身搬到别的边（那会破坏四边布局 → 左/右被误搬到下方、图标重叠、
    // 四边表现不一致）。SetDockPosition 仍保留给右键菜单"换边"功能（Step 13），此处不再调用。
    if (m_owner->m_window && (m_owner->m_mousePenetrating || m_owner->m_autoHide ||
                               m_owner->AnyScaleElevated())) {
        POINT pt;
        if (!GetCursorPos(&pt)) return;
        RECT dr      = m_owner->m_window->GetDockRect();
        RECT fullWin = m_owner->m_window->GetFullWindowRect();
        bool inDock  = PtInRect(&dr, pt) != 0;
        bool inFull  = PtInRect(&fullWin, pt) != 0;

        // P0-4：四角→IDLE 硬约束（《动作执行规范》§2）。屏幕四角为相邻两边感应带
        // 法向厚度交叠正方形，光标落于角格不得唤出任何 Dock。在 reveal 判定最前序拦截。
        RECT workArea = m_owner->m_window ? m_owner->m_window->GetWorkArea()
                                          : RECT{ 0, 0,
                                                  GetSystemMetrics(SM_CXVIRTUALSCREEN),
                                                  GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        bool inCorner = HitTestEngine::IsInCornerIdleZone(pt, workArea,
                                                          m_owner->ComputeCornerSize());

        // #1 兜底：光标离开整窗且不在【本边】感应区，且鱼眼仍放大 → 复位（防 WM_MOUSELEAVE
        // 漏发卡死）。ComputeRevealZoneFor(m_appConfig.dock.position, dr) 对本边是正确的
        // （dr 即本边矩形），IsEdgeEnabled(本边) 对本引擎恒为 true（创建时即启用）。
        RECT ownReveal = m_owner->ComputeRevealZoneFor(m_owner->m_appConfig.dock.position, dr);
        bool inOwnReveal = m_owner->IsEdgeEnabled(m_owner->m_appConfig.dock.position)
                           && (PtInRect(&ownReveal, pt) != 0);
        // Bugfix：拖拽删除 —— 拖拽进行中且光标已离开本边感应区，立即删除图标。
        // 复用 inOwnReveal 语义（与 autoHide reveal 一致），不另定义感应区。
        // 守卫：m_dragMoved 为真（已明显位移）才触发，纯点击不误伤；重排时光标始终
        // 在 Dock 内（∈ 感应区）→ inOwnReveal 为真 → 不触发；删除后置 m_dragging=false，
        // 后续 WM_LBUTTONUP 因 m_dragging 为假直接 no-op，杜绝双删。
        // （真实 GUI：拖出窗口后 HandleMouseMove→HandleMouseLeave 设 m_mousePenetrating=true，
        //  看门狗持续运行 → WM_APP_IDLE → TickIdle 每帧可命中此分支。）
        // Bugfix（用户报障 B：「点击启动后鼠标仍被误判为图标拖拽 → 离开感应区即删除」）：
        // 物理主键守卫 —— m_dragging 的语义是「主键按住期间的拖拽手势进行中」，
        // 主键一旦松开，任何残留的 m_dragging 都是脏状态，绝不允许再驱动删除路径。
        // 脏状态的真实来源（已复核）：WM_LBUTTONUP 内 LaunchIcon → ShellExecuteW 是
        // 同步阻塞调用，且会为 DDE / Shell 会话【泵消息】；泵消息期间窗口仍持有
        // SetCapture、m_dragging 仍为 true，用户在应用启动过程中移动鼠标 → 嵌套派发的
        // WM_MOUSEMOVE 把 m_dragMoved 置真 → 同样被嵌套派发的 WM_APP_IDLE 落到下面的
        // 拖拽删除分支，图标就在「启动结束、鼠标离开感应区」的瞬间被删掉。
        // 用 GetAsyncKeyState 取【物理】键态：GetKeyState 取的是消息队列同步态，
        // 恰恰在 WM_LBUTTONUP 丢失/迟到的场景下不可信。左右键互换时主键为 VK_RBUTTON。
        {
            const int  primaryVk   = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
            const bool primaryDown = (GetAsyncKeyState(primaryVk) & 0x8000) != 0;
            if (m_owner->m_dragging && !primaryDown) {
                m_owner->m_dragging  = false;
                m_owner->m_dragMoved = false;
                m_owner->m_dragIndex = -1;
            }
        }
        // BUG3 删除守卫（第二条删除路径）：除 >= 0 外必须再校验上界。m_dragIndex 是在
        // WM_LBUTTONDOWN 时按下点命中的下标，拖拽期间若有其它路径增删图标（右键菜单 /
        // 拖入添加），该下标可能已越界，直接 RemoveIcon 会删错图标甚至越界。
        // 与 WM_LBUTTONUP 的守卫同语义：任何不合法下标一律放弃删除。
        const bool dragIndexValid =
            (m_owner->m_dragIndex >= 0 && m_owner->m_dragIndex < m_owner->GetIconCount());
        // 手势成立（拖拽中 + 已位移 + 已离开本边感应区）才进入删除决策。
        const bool dragGesture =
            (m_owner->m_dragging && m_owner->m_dragMoved && !inOwnReveal);
        if (dragGesture && !dragIndexValid) {
            // 手势成立但下标不可信 → 只收拾拖拽态，不删任何图标。
            m_owner->m_dragging  = false;
            m_owner->m_dragMoved = false;
            m_owner->m_dragIndex = -1;
        } else if (dragGesture) {
            m_owner->RemoveIcon(m_owner->m_dragIndex, true);
            m_owner->m_dragging   = false;
            m_owner->m_dragMoved  = false;
            m_owner->m_dragIndex  = -1;
            ReleaseCapture();     // 释放鼠标捕获（真实 GUI：拖拽中 SetCapture 已捕获）
            m_owner->ApplyRestTargets();   // 复位视觉（其余图标回归静息 scale=1）
            m_owner->StartAnimationLoop(); // 驱动回落动画
        }

        // #7 修复：移除 !inFull 门槛。ComputeInsets 使整窗向屏内扩展
        // halfGrowth+tooltipPad（底边约 96px），而 reveal 带仅扩展 dockHeight（约 48-72px），
        // 故整窗矩形完全包住 reveal 带。若保留 !inFull，光标落在 reveal 带内（且通常在整窗内）
        // 时 inFull 恒 true → !inFull 恒 false → inOwnReveal 恒 false → 自动隐藏四条边的感应区
        // 全部唤不出。移除后，reveal 带（含整窗透明区）即可触发 Show，符合预期手感。

        // Bugfix（用户报障 #2「离开图标但未离开感应区时过度缩小」）：
        // 原条件含 !inOwnReveal —— 光标离开整窗但仍在本边感应带内时【不复位】，鱼眼冻结在
        // 放大态；而一旦再走出感应带则走下方 Hide() → ApplyExitTargets() 把 scale 打到 0
        // （视觉上「缩到最小」）。二者都不是期望行为。
        // 正解：光标只要不在整窗内（!inFull），就不可能悬停任何图标，鱼眼应回到【正常大小】
        // (ApplyRestTargets → scale=1.0)；是否仍在感应带内只决定「要不要收起 Dock」
        // （见下方 inOwnReveal 分支取消隐藏倒计时），与缩放目标无关，故此处不再耦合。
        if (!inFull && m_owner->AnyScaleElevated()) {
            m_owner->ApplyRestTargets();
            m_owner->StartAnimationLoop();
        }

        // Bugfix（用户报障 A：四条边一致的「显示区内唤不出、只有快速划入感应区才醒」）：
        // 隐藏 + autoHide 时，唤出判定必须【前置】，不能被 inDock 分支吞掉。
        // 根因链（已复核）：
        //   1) 隐藏只是 WindowManager::Show(false) → ShowWindow(SW_HIDE)，m_baseRect 原地不动，
        //      故 GetDockRect()（= 用户口中的「显示区」）在隐藏态依然覆盖屏幕边缘那一条；
        //   2) ComputeRevealZoneFor 是「dockRect 沿法向朝屏内扩 dockHeight/dockWidth」，
        //      即 revealZone ⊇ dockRect —— 显示区本就是感应区的子集；
        //   3) 旧分支顺序下，光标只要落进 dockRect，inDock 恒为真 → 走 HandleMouseMove，
        //      而该函数首行对 Hidden/Exiting 直接 return（DockInteraction.cpp:51）→ 什么都不做，
        //      且 else-if 的唤出分支【永远到不了】。
        //   结论：只有「朝屏内快速划过 dockRect、被 100ms 看门狗采样恰好抓在扩展带里」
        //   才唤得出；把光标静置在显示区（底/顶/左/右四边同理）则永远唤不出 —— 与报障完全吻合。
        // 修正：Hidden+autoHide 优先做唤出判定，判据取 (inOwnReveal ∪ inDock)。
        // 并集里的 inDock 是冗余保险：正常情况下它已被 inOwnReveal 包含，但 reveal 带在
        // work 边界处会被 clamp（见 ComputeRevealZoneFor），dock 若被 edgeOffset 推到工作区
        // 之外就会出现 dockRect ⊄ revealZone 的退化情形，此时显示区仍须能唤出。
        // 四角硬约束 !inCorner 保持不变（语义仍只存在于这一处）。
        const bool hiddenAutoHide =
            (m_owner->m_autoHide && m_owner->m_state == DockState::Hidden);
        if (hiddenAutoHide) {
            // #3（多引擎修正）：仅当光标进入【本边】感应区/显示区才唤起本 dock。
            // 不再跨边判定、不再 SetDockPosition —— 每条边是独立永久 dock。
            const bool inOwnZone =
                m_owner->IsEdgeEnabled(m_owner->m_appConfig.dock.position)
                && (inOwnReveal || inDock);
            if (inOwnZone && !inCorner) {
                // 隐藏态也记录光标位置，使 Show() 后的首帧鱼眼主轴坐标不是上一次的陈旧值。
                m_owner->m_lastMousePos = pt;
                if (m_owner->m_showDelayMs <= 0) {
                    m_owner->Show();
                } else if (m_owner->m_showCountdown <= 0.0f) {
                    m_owner->m_showCountdown = m_owner->m_showDelayMs / 1000.0f;
                    m_owner->m_probeHoverPending = true;
                }
            }
        } else if (inDock) {
            // 直接命中 Dock 区域：恢复交互（清除穿透，转悬停）
            m_owner->HandleMouseMove(pt.x, pt.y);
        } else {
            // Bug #2 默认隐藏兜底：autoHide 显示后，光标停在穿透透明区
            // （HTTRANSPARENT）时收不到 WM_MOUSELEAVE；看门狗虽持续运行（m_autoHide
            // 恒真），但原逻辑只有 inDock / inOwnReveal(Hidden) 两条路径，光标在透明区
            // 逛一圈离开后无任何路径调 Hide()，dock 永久停留、不收起。
            // 此处补：可见态且光标不在整窗、也不在本边感应区 → 启动隐藏倒计时
            // （m_hideDelayMs<=0 立即 Hide，否则 m_hideCountdown=m_hideDelayMs/1000）；
            // 若光标仍在【本边】感应区（inOwnReveal）且可见 → 取消待隐藏，
            // 避免回到感应区仍被误收起。与上方 !inFull && !inOwnReveal &&
            // AnyScaleElevated() 复位块共存、互不干扰。
            bool visibleState = (m_owner->m_state != DockState::Hidden &&
                                 m_owner->m_state != DockState::Exiting);
            if (m_owner->m_autoHide && visibleState) {
                if (inOwnReveal) {
                    m_owner->m_hideCountdown = 0.0f;   // 仍在感应区：取消待隐藏
                } else if (!inFull) {
                    // 完全离开（不在整窗、不在本边感应区）→ 启动隐藏倒计时
                    if (m_owner->m_hideDelayMs <= 0) {
                        m_owner->Hide();
                    } else if (m_owner->m_hideCountdown <= 0.0f) {
                        m_owner->m_hideCountdown = m_owner->m_hideDelayMs / 1000.0f;
                    }
                }
                // 其余：inFull && !inOwnReveal（光标在本窗透明留白内逛、未放大）
                // 暂不隐藏；待其离开整窗（inFull 变 false）即进入上一分支启动倒计时。
            }
        }
    }
    // #1：看门狗自维护——若鱼眼已复位且非自动隐藏/穿透态，停止 100ms 轮询以省 CPU。
    m_owner->UpdateIdleWatchdog();
}

void DockStateMachine::AdvanceAutoHide(float dt) {
    if (m_owner->m_showCountdown > 0.0f) {
        m_owner->m_showCountdown -= dt;
        if (m_owner->m_showCountdown <= 0.0f) {
            m_owner->m_showCountdown = 0.0f;
            if (m_owner->m_probeHoverPending) {
                m_owner->m_probeHoverPending = false;
                if (m_owner->m_state == DockState::Hidden) m_owner->Show();   // 显示延迟到期 → 弹出
            }
        }
    }
    if (m_owner->m_hideCountdown > 0.0f) {
        m_owner->m_hideCountdown -= dt;
        if (m_owner->m_hideCountdown <= 0.0f) {
            m_owner->m_hideCountdown = 0.0f;
            if (m_owner->m_state != DockState::Hidden && m_owner->m_state != DockState::Exiting) {
                m_owner->Hide();   // 隐藏延迟到期 → 收起
            }
        }
    }
}

RECT DockStateMachine::ComputeRevealZone() const {
    RECT r = {};
    if (m_owner->m_window) r = m_owner->m_window->GetDockRect();
    return m_owner->ComputeRevealZoneFor(m_owner->m_appConfig.dock.position, r);
}

// ═══════════════════════════════════════════════════════════
// 自动隐藏 + 边开关（#3）
// ═══════════════════════════════════════════════════════════

void DockStateMachine::SetAutoHideEnabled(bool on) {
    m_owner->m_autoHide = on;
    if (!on) m_owner->m_hideCountdown = 0.0f;   // 关闭时取消待执行的隐藏
    m_owner->UpdateIdleWatchdog();
}

RECT DockStateMachine::ComputeRevealZoneFor(DockPosition edge, RECT r) const {
    RECT work = GetMonitorWorkRect(r);
    switch (edge) {
    case DockPosition::Top:
        r.left   = work.left;                            // 横向全屏（主轴）
        r.right  = work.right;
        r.bottom = r.bottom + (LONG)m_owner->m_dockHeight;        // 向屏内（下）扩展
        if (r.bottom > work.bottom) r.bottom = work.bottom;
        break;
    case DockPosition::Bottom:
        r.left   = work.left;                            // 横向全屏（主轴）
        r.right  = work.right;
        r.top    = r.top - (LONG)m_owner->m_dockHeight;           // 向屏内（上）扩展
        if (r.top < work.top) r.top = work.top;
        break;
    case DockPosition::Left:
        r.top    = work.top;                             // 纵向全屏（主轴）
        r.bottom = work.bottom;
        // Bugfix：原写作 r.left -= dockWidth（朝屏【外】扩），随后又被 clamp 回 work.left，
        // 净效果为「零扩展」→ 感应区退化成 Dock 条本身，左边几乎唤不出。与 Top/Bottom
        // 对称，屏内方向是【右】，故应扩 r.right。
        r.right  = r.right + (LONG)m_owner->m_dockWidth;         // 向屏内（右）扩展
        if (r.right > work.right) r.right = work.right;
        break;
    case DockPosition::Right:
        r.top    = work.top;                             // 纵向全屏（主轴）
        r.bottom = work.bottom;
        // Bugfix：同上，原 r.right += dockWidth 朝屏【外】扩并被 clamp 回 work.right，
        // 净零扩展。右边的屏内方向是【左】，故应扩 r.left。
        r.left   = r.left - (LONG)m_owner->m_dockWidth;          // 向屏内（左）扩展
        if (r.left < work.left) r.left = work.left;
        break;
    }
    return r;
}

// #3：四边吸附/感应区独立开关

bool DockStateMachine::IsEdgeEnabled(DockPosition edge) const {
    // #N：以权威开关 edgeEnabled 数组为准（索引=DockPosition: Bottom/Top/Left/Right）
    return m_owner->m_appConfig.edgeEnabled[(int)edge];
}

DockPosition DockStateMachine::NextEnabledEdge(DockPosition avoid) const {
    DockPosition order[] = { DockPosition::Bottom, DockPosition::Top,
                             DockPosition::Left, DockPosition::Right };
    for (DockPosition e : order)
        if (e != avoid && m_owner->IsEdgeEnabled(e)) return e;
    return avoid;   // 无其它启用边
}

bool& DockStateMachine::EdgeRef(DockPosition edge) {
    return m_owner->m_appConfig.edgeEnabled[(int)edge];
}

void DockStateMachine::ApplyDockPosition(DockPosition pos) {
    // #4：保存当前边图标集，载入目标边独立图标集（每边拥有自己的应用/文件夹）
    m_owner->SyncCurrentEdgeIcons();   // 保存当前边图标集（含共享存储）
    m_owner->m_appConfig.dock.position = pos;
    m_owner->m_geom = MakeGeometry(pos);   // 统一几何：运行时切换停靠边 → 重建多态几何实例
    m_owner->m_appConfig.icons = !m_owner->m_appConfig.edgeIcons[(int)pos].empty()
        ? m_owner->m_appConfig.edgeIcons[(int)pos] : m_owner->m_appConfig.sharedIcons;

    // 先同步渲染器朝向与 Dock 尺寸（须在 ApplyPlacement→SetContentInsets 之前，
    // 否则渲染器用旧尺寸算留白/画布，且 UpdateVisualTransforms 仍走旧朝向分支）。
    if (m_owner->m_render) m_owner->m_render->SetDockPosition(pos);

    // 重建弹簧/纹理/布局/定位（图标数量与朝向可能随边改变），并持久化（SaveConfig 写 edgeIcons）
    m_owner->RebuildIcons(true);
}
