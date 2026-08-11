// src/app/DockInteraction.h
// DockEngine 子模块：窗口消息 / 鼠标 / 点击 / 命中测试 / 拖放 / 右键菜单 / 托盘 / 诊断
// 方法体见 DockInteraction.cpp；所有 DockEngine 成员经 m_owner-> 访问（friend）。
#pragma once
#include "DockEngine.h"   // POINT / HWND / LRESULT 等类型

// 子模块经 m_owner（DockEngine*）反查宿主私有成员，故 DockEngine 需声明本类为 friend。
class DockInteraction {
public:
    explicit DockInteraction(DockEngine* owner) : m_owner(owner) {}
    DockEngine* m_owner;

    // ═══ 消息处理（Windowed）═══
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ═══ 交互处理 ═══
    void HandleMouseMove(int screenX, int screenY);
    void HandleMouseLeave();
    void HandleClick(int screenX, int screenY);

    // BUG3：在【给定屏幕点】做一次即时命中测试，返回图标下标（-1 = 未命中）。
    //
    // 存在理由（务必读完再改）：m_hoveredIndex 是【视觉悬停态】，它被 HandleMouseMove
    // 刻意做成「粘滞」的 —— 光标滑到图标间隙/留白时【不清零】，好让 tooltip 与鱼眼
    // 不闪烁（见 HandleMouseMove 的 else 分支注释 #5/#7）。粘滞对视觉是对的，
    // 但对【动作】（启动/拖拽/删除）是致命的：它会把上一帧的旧下标一直保留下来，
    // 于是「按下点其实没命中任何图标」也会读出一个真实下标 → 启动错的应用、
    // 更糟的是拖拽删除会删掉一个用户根本没碰的图标（真机 4→3→2）。
    //
    // 因此：视觉用 m_hoveredIndex（粘滞），动作一律改用本函数（无状态、即时、
    // 就地按下点重算）。二者职责分离，互不污染。
    int ResolveHitIndexAt(POINT pt) const;

    // 右键菜单已移除（需求：去掉四个边的右键菜单）；HandleMenuCommand 现无调用方，保留接口
    void HandleMenuCommand(int cmd);                  // Step 13：Dock 菜单命令分发

    // ═══ 系统托盘图标（仅 Windowed 模式）═══
    void AddTrayIcon();
    void RemoveTrayIcon();
};
