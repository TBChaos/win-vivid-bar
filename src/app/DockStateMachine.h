// src/app/DockStateMachine.h
// DockEngine 子模块：动画状态机 / 自动隐藏看门狗 / 边切换
// 方法体见 DockStateMachine.cpp；所有 DockEngine 成员经 m_owner-> 访问（friend）。
#pragma once
#include "DockEngine.h"   // DockState / DockPosition / RECT 等类型

// 子模块经 m_owner（DockEngine*）反查宿主私有成员，故 DockEngine 需声明本类为 friend。
class DockStateMachine {
public:
    explicit DockStateMachine(DockEngine* owner) : m_owner(owner) {}
    DockEngine* m_owner;

    // ═══ 状态机 ═══
    void EnterState(DockState next);
    void UpdateStateMachine(bool allSettled);

    // ═══ 自动隐藏 + 空闲鼠标穿透（Step 7）═══
    void SetAutoHideEnabled(bool on);
    void SetPenetration(bool penetrate);
    // P0 遮挡挂起：置/清遮挡态。on 时停动画循环并（经 UpdateIdleWatchdog）停看门狗；
    // off 时按原判据恢复看门狗。幂等（同值直接返回）。
    void SetOccluded(bool on);
    void UpdateIdleWatchdog();          // 依据 !遮挡 && (autoHide || 穿透 || 放大) 启停看门狗
    void StartWatchdog();
    void StopWatchdog();
    void TickIdle(float dt);            // 看门狗回调：推进显示/隐藏倒计时 + 探测光标
    void AdvanceAutoHide(float dt);     // 推进 show/hide 倒计时（动画帧与看门狗共用）

    // ═══ 边缘感应区 / 边开关（#3）═══
    RECT ComputeRevealZone() const;
    RECT ComputeRevealZoneFor(DockPosition edge, RECT r) const;  // #3 指定边
    DockPosition NextEnabledEdge(DockPosition avoid) const;      // #3 下一个启用边
    bool& EdgeRef(DockPosition edge);                            // #3 边开关引用
    bool IsEdgeEnabled(DockPosition edge) const;                 // #3 该边是否启用（只读）

    // P1-7：受锁保护的换边实现（SetDockPosition/SetEdgeEnabled 内部调用，避免重入死锁）
    void ApplyDockPosition(DockPosition pos);
};
