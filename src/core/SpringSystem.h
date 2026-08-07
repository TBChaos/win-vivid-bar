// src/core/SpringSystem.h
// 弹簧物理系统 — 半隐式欧拉积分（设计参考：详细设计说明 §2.2）
#pragma once
#include "../Common.h"

// ═══════════════════════════════════════════════════════════
// 弹簧参数
// ═══════════════════════════════════════════════════════════
struct SpringParams {
    float stiffness = 300.0f;   // 刚度 k（N/m）
    float damping   = 20.0f;    // 阻尼 c（N·s/m）
    float mass      = 1.0f;     // 质量 m（kg）

    static SpringParams Hover()  { return { 300.0f, 20.0f, 1.0f }; }  // 快速响应，轻微过冲
    static SpringParams Bounce() { return { 180.0f, 12.0f, 1.0f }; }  // 明显弹跳
    static SpringParams Entry()  { return { 200.0f, 18.0f, 1.0f }; }  // 平滑入场
    static SpringParams Exit()   { return { 400.0f, 30.0f, 1.0f }; }  // 快速退出
    static SpringParams Rest()   { return { 800.0f, 56.0f, 1.0f }; }  // 快速复位（临界阻尼，无过冲拖尾）
};

// ═══════════════════════════════════════════════════════════
// 弹簧实例
// ═══════════════════════════════════════════════════════════
struct SpringInstance {
    float value    = 0.0f;   // 当前值
    float velocity = 0.0f;   // 当前速度
    float target   = 0.0f;   // 目标值
    SpringParams params;     // 当前参数

    static constexpr float VALUE_THRESHOLD    = 0.001f;
    static constexpr float VELOCITY_THRESHOLD = 0.01f;

    bool IsSettled() const {
        return std::abs(value - target) < VALUE_THRESHOLD
            && std::abs(velocity) < VELOCITY_THRESHOLD;
    }
    void SnapToTarget() { value = target; velocity = 0.0f; }
};

// ═══════════════════════════════════════════════════════════
// 弹簧系统
// ═══════════════════════════════════════════════════════════
class SpringSystem {
public:
    uint32_t CreateSpring(float initialValue, const SpringParams& params);
    void  Remove(uint32_t id);            // P0-3：按 id 摘除单个弹簧（增量重建用）
    void  SetTarget(uint32_t id, float target);
    void  SetParams(uint32_t id, const SpringParams& params);
    void  SetValue(uint32_t id, float value);          // 直接设置当前值（初始化用）
    float GetValue(uint32_t id) const;
    float GetVelocity(uint32_t id) const;
    float GetTarget(uint32_t id) const;
    bool  IsSettled(uint32_t id) const;

    // 更新所有弹簧，返回是否全部收敛
    bool Update(float dt);

    // 查询：所有弹簧是否已收敛（验收用，不推进积分）
    bool AllSettled() const;

    size_t Count() const { return m_springs.size(); }

    // 调试：获取所有弹簧状态
    std::vector<std::pair<uint32_t, SpringInstance>> GetAllSprings() const;

private:
    std::unordered_map<uint32_t, SpringInstance> m_springs;
    uint32_t m_nextId = 0;
};
