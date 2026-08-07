// src/platform/WindowManager.h
// 窗口管理 — WS_POPUP 分层透明置顶窗口（设计参考：详细设计说明 §四）
#pragma once
#include "../Common.h"

class WindowManager {
public:
    ~WindowManager() { Destroy(); }

    // debugAtOrigin: 调试模式下窗口放在 (0,0)（几何确定，便于模拟测试）
    HRESULT Create(HINSTANCE hinstance, int width, int height,
                   WNDPROC wndProc, void* userData, bool debugAtOrigin);
    void Destroy();

    void Show(bool visible);
    // ═══ Step 7：空闲鼠标穿透（WM_NCHITTEST 区域判定，非全局 WS_EX_TRANSPARENT）═══
    // 旧实现 SetMousePenetration 为全局 WS_EX_TRANSPARENT 空实现（P0-5 已删除）：
    // 全局穿透会让整个窗口（含 Dock 条）对命中测试透明，导致右键菜单/拖拽添加无法送达。
    // 穿透语义现由 DockEngine::WM_NCHITTEST 按区域判定（Dock 条→HTCLIENT，留白→HTTRANSPARENT），
    // 不再提供任何公共穿透 API。此处保留 deleted 声明以显式排除误用。
    void SetMousePenetration(bool) = delete;   // P0-5：空实现公共 API 已移除
    bool IsMousePenetrating() const { return m_penetrating; }
    bool IsVisible() const { return m_visible; }

    void PositionAtBottomCenter(int width, int height);   // 工作区底部居中

    // ═══ Acrylic 毛玻璃 + 窗口圆角（D方案 §2.1）═══
    // 启用背景模糊。tintARGB 为 AABBGGRR 叠色（仅 Acrylic 模式使用）。
    // 返回实际生效方式：0=未启用 1=Acrylic 2=Accent Blur 3=DwmBlurBehind
    int  ApplyBackgroundBlur(bool enable, unsigned int tintABGR);
    // 声明合成路径：true=DirectComposition（DComp 逐像素 alpha 合成，禁用 Acrylic 分支）；
    // false=GDI 回退（可走 SetWindowCompositionAttribute / DwmBlurBehind 兜底模糊）。
    void SetCompositionOwner(bool dcomp);
    bool ApplyRoundedCorners();     // Win11 DWMWA_WINDOW_CORNER_PREFERENCE；Win10 返回 false
    int  GetBlurMode() const { return m_blurMode; }
    bool IsRounded()    const { return m_rounded;  }

    // ═══ DPI / 多显示器（Step 6）═══
    // 指定显示器（0 起）的工作区底部居中；越界回退主显示器
    void RepositionBottomCenter(int width, int height, int monitorIndex);

    // ═══ Step 10：位置微调 + 图层 Z 序 ═══
    // 通用停靠定位：edge 0=bottom 1=top 2=left 3=right；
    // edgeOffset = 距停靠边的偏移（px，向屏幕内为正）；
    // centerOffset = 沿停靠边的居中偏移（px，正值向右/向下）。
    void RepositionDock(int width, int height, int monitorIndex,
                        int edge, int edgeOffset, int centerOffset);
    // 图层层级：1=总在前面(TOPMOST) 0=正常(NOTOPMOST) -1=总在后面(BOTTOM)
    void ApplyZOrder(int zOrder);
    int  GetZOrder() const { return m_zOrder; }
    UINT GetDpi() const;                 // 窗口 DPI（Win10 1607+；否则 96）
    static int GetMonitorCount();        // 系统显示器数量（>=1）

    // ═══ Step 12：放大溢出边距（内容相对窗口的留白）═══
    // 窗口尺寸 = 基础 Dock 条尺寸 + 四边留白；图标放大/tooltip 在留白内绘制，不再被裁切。
    // GetDockRect() 返回的是「基础 Dock 条」矩形（窗口内缩四边留白），用于命中/感应区判定。
    void SetContentInsets(int left, int top, int right, int bottom);

    HWND GetHwnd() const { return m_hwnd; }
    RECT GetDockRect() const;                              // 屏幕坐标「基础 Dock 条」矩形
    RECT GetFullWindowRect() const;                        // 屏幕坐标「含留白整窗」矩形（#5/#7）
    // P0-4：当前显示器工作区（排除任务栏）；无窗口（无头）时回退主显示器工作区。
    // 供 TickIdle 四角→IDLE 判定注入工作区。
    RECT GetWorkArea() const;

private:
    HWND      m_hwnd = nullptr;
    HINSTANCE m_hinstance = nullptr;
    bool      m_classRegistered = false;
    int       m_blurMode = 0;       // 0=off 1=acrylic 2=accent blur 3=dwm blur
    bool      m_dcompOwned = false; // true=DirectComposition 路径（禁用 Acrylic，由 DComp 视觉提供模糊）
    bool      m_rounded  = false;
    bool      m_visible    = false;   // 窗口当前是否可见（Show 维护）
    bool      m_penetrating = false;  // 当前是否处于鼠标穿透（WS_EX_TRANSPARENT）
    int       m_zOrder     = -1;      // 当前 Z 序（1=TOPMOST 0=normal -1=bottom，默认总在后面）

    // Step 12：内容留白（窗口相对基础 Dock 条的内缩量）
    int       m_insetL = 0, m_insetT = 0, m_insetR = 0, m_insetB = 0;
    int       m_baseW  = 0, m_baseH  = 0;   // 基础 Dock 条尺寸（不含留白）
    RECT      m_baseRect = { 0,0,0,0 };     // 基础 Dock 条屏幕矩形

    // 由 m_zOrder 求 SetWindowPos 的 insertAfter 句柄
    HWND ZInsertAfter() const;

    static constexpr const wchar_t* WINDOW_CLASS = L"openDockWindow";
};
