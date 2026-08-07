// src/core/SpringSystem.cpp
#include "SpringSystem.h"

uint32_t SpringSystem::CreateSpring(float initialValue, const SpringParams& params) {
    uint32_t id = m_nextId++;
    SpringInstance spring;
    spring.value    = initialValue;
    spring.target   = initialValue;
    spring.velocity = 0.0f;
    spring.params   = params;
    m_springs[id] = spring;
    return id;
}

void SpringSystem::Remove(uint32_t id) {
    // P0-3：按 id 摘除单个弹簧（增量重建时仅移除真正消失的图标节点）。
    // 不重建整个系统，稳定图标的 value/velocity 连续保留。
    m_springs.erase(id);
}

void SpringSystem::SetTarget(uint32_t id, float target) {
    auto it = m_springs.find(id);
    if (it != m_springs.end()) {
        it->second.target = target;
    }
}

void SpringSystem::SetParams(uint32_t id, const SpringParams& params) {
    auto it = m_springs.find(id);
    if (it != m_springs.end()) {
        it->second.params = params;
    }
}

void SpringSystem::SetValue(uint32_t id, float value) {
    auto it = m_springs.find(id);
    if (it != m_springs.end()) {
        it->second.value = value;
        it->second.velocity = 0.0f;
    }
}

float SpringSystem::GetValue(uint32_t id) const {
    auto it = m_springs.find(id);
    return (it != m_springs.end()) ? it->second.value : 0.0f;
}

float SpringSystem::GetVelocity(uint32_t id) const {
    auto it = m_springs.find(id);
    return (it != m_springs.end()) ? it->second.velocity : 0.0f;
}

float SpringSystem::GetTarget(uint32_t id) const {
    auto it = m_springs.find(id);
    return (it != m_springs.end()) ? it->second.target : 0.0f;
}

bool SpringSystem::IsSettled(uint32_t id) const {
    auto it = m_springs.find(id);
    return (it != m_springs.end()) ? it->second.IsSettled() : true;
}

bool SpringSystem::Update(float dt) {
    bool allSettled = true;

    for (auto& [id, spring] : m_springs) {
        (void)id;
        if (spring.IsSettled()) {
            spring.SnapToTarget();
            continue;
        }

        // 半隐式欧拉积分（Symplectic Euler）：
        // 先更新速度，再用新速度更新位置
        float displacement = spring.value - spring.target;
        float springForce  = -spring.params.stiffness * displacement;
        float dampingForce = -spring.params.damping * spring.velocity;
        float acceleration = (springForce + dampingForce) / spring.params.mass;

        spring.velocity += acceleration * dt;
        spring.value    += spring.velocity * dt;

        if (!spring.IsSettled()) {
            allSettled = false;
        }
    }

    return allSettled;
}

bool SpringSystem::AllSettled() const {
    // 空集合 = 无动画在途 = 已收敛（与 Update() 的语义保持一致：空 map 返回 true）。
    // 否则「没有任何弹簧」会被误判为「未收敛」，导致状态机（如 ENTERING→IDLE）卡死。
    for (const auto& [id, spring] : m_springs) {
        (void)id;
        if (!spring.IsSettled()) return false;
    }
    return true;
}

std::vector<std::pair<uint32_t, SpringInstance>> SpringSystem::GetAllSprings() const {
    std::vector<std::pair<uint32_t, SpringInstance>> result;
    result.reserve(m_springs.size());
    for (const auto& kv : m_springs) {
        result.push_back(kv);
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return result;
}
