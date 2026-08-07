// src/platform/WindowManager.cpp
#include "WindowManager.h"
#include <dwmapi.h>

// ═══ SetWindowCompositionAttribute（未公开 API，D方案 §2.1 Acrylic）═══
namespace {

enum ACCENT_STATE_E {
    ACCENT_DISABLED_E                 = 0,
    ACCENT_ENABLE_BLURBEHIND_E        = 3,   // Win10 1607+ 模糊
    ACCENT_ENABLE_ACRYLICBLURBEHIND_E = 4,   // Win10 1803+ 亚克力
};

struct ACCENT_POLICY_T {
    int          AccentState;
    int          AccentFlags;
    unsigned int GradientColor;   // AABBGGRR
    int          AnimationId;
};

struct WCA_DATA_T {
    int    Attrib;                // 19 = WCA_ACCENT_POLICY
    PVOID  pvData;
    SIZE_T cbData;
};

typedef BOOL(WINAPI* PFN_SetWindowCompositionAttribute)(HWND, WCA_DATA_T*);
constexpr int WCA_ACCENT_POLICY_V = 19;

// ═══ 显示器枚举辅助（Step 6）═══
BOOL CALLBACK CountMonitorsProc(HMONITOR, HDC, LPRECT, LPARAM lp) {
    ++(*reinterpret_cast<int*>(lp));
    return TRUE;
}

struct MonitorFindCtx {
    int      target = 0;
    int      cur    = 0;
    HMONITOR hmon   = nullptr;
};

BOOL CALLBACK FindMonitorProc(HMONITOR h, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<MonitorFindCtx*>(lp);
    if (ctx->cur++ == ctx->target) {
        ctx->hmon = h;
        return FALSE;   // 找到即停止
    }
    return TRUE;
}

}  // namespace

HRESULT WindowManager::Create(HINSTANCE hinstance, int width, int height,
                              WNDPROC wndProc, void* userData, bool debugAtOrigin) {
    m_hinstance = hinstance ? hinstance : GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = m_hinstance;
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = nullptr;   // 无背景，完全由 DComp 合成
    wc.lpszClassName = WINDOW_CLASS;

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return HRESULT_FROM_WIN32(err);
        }
    }
    m_classRegistered = true;

    // 位置：调试模式 (0,0)；正常模式工作区底部居中
    int x = 0, y = 0;
    if (!debugAtOrigin) {
        RECT workArea = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
        y = workArea.bottom - height;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
        WINDOW_CLASS, L"openDock",
        WS_POPUP,
        x, y, width, height,
        nullptr, nullptr, m_hinstance, userData);

    if (!m_hwnd) {
        DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err);
    }

    // 去除 DWM 在窗口四周绘制的 1px 系统边框（Win11 默认对非分层 WS_POPUP +
    // WS_EX_NOREDIRECTIONBITMAP + DComp 窗口描的边，光靠窗口样式去不掉）。
    // 标准解法：把 DWMWA_BORDER_COLOR(34) 与 DWMWA_VISIBLE_FRAME_BORDER_COLOR(37)
    // 都设为 DWMWA_COLOR_NONE(0xFFFFFFFE)。Win10 不支持该属性，调用失败即忽略，无副作用。
    {
        const DWORD attrBorderColor = 34;            // DWMWA_BORDER_COLOR
        const DWORD attrVisibleFrame = 37;           // DWMWA_VISIBLE_FRAME_BORDER_COLOR
        const DWORD borderNone      = 0xFFFFFFFE;    // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(m_hwnd, attrBorderColor,    &borderNone, sizeof(borderNone));
        DwmSetWindowAttribute(m_hwnd, attrVisibleFrame,   &borderNone, sizeof(borderNone));
    }

    m_baseW = width; m_baseH = height;
    m_baseRect = { x, y, x + width, y + height };

    // 注意：本窗口【不带 WS_EX_LAYERED】，是普通（非分层）窗口 + WS_EX_NOREDIRECTIONBITMAP
    // + DirectComposition。逐像素透明度由 DComp 视觉的 alpha 经 NOREDIRECTIONBITMAP 提供，
    // 无需分层窗口参与。
    //   为什么必须去掉 WS_EX_LAYERED：WS_EX_LAYERED 与 WS_EX_NOREDIRECTIONBITMAP 语义互斥 ——
    //   前者要求窗口拥有一张分层表面（由 SetLayeredWindowAttributes / UpdateLayeredWindow 填充），
    //   后者恰恰声明「本窗口没有重定向位图」。二者同时挂上时分层表面恒为空，命中测试阶段
    //   WindowFromPoint 会直接跳过本 HWND → OS 根本不向窗口投递鼠标消息 → 四边图标全部点不到
    //   （点击穿透到桌面）。
    //   去掉后：OS 正常投递鼠标消息，WM_NCHITTEST 处理器（DockInteraction → HitTestAt 返回
    //   HTCLIENT / HTTRANSPARENT）立即生效 —— 透明留白区仍按 HTTRANSPARENT 穿透，
    //   Dock 条与图标区按 HTCLIENT 可点。
    // 【严禁】在此调用 SetLayeredWindowAttributes：它会把整窗标记为一个不透明(255)的分层表面，
    // 导致 alpha=0 的像素（Dock 条以外的全部区域）被 DWM 合成成黑色 —— 即历史上用户看到的
    // 「用途不明的大黑框」。
    // GDI 回退路径（RenderManager::InitializeGDI）会在需要时自行改挂 WS_EX_LAYERED 并移除
    // WS_EX_NOREDIRECTIONBITMAP，再用 UpdateLayeredWindow 提供位图，与此处互不冲突。

    return S_OK;
}

void WindowManager::SetContentInsets(int left, int top, int right, int bottom) {
    m_insetL = left; m_insetT = top; m_insetR = right; m_insetB = bottom;
    // 初始化阶段（窗口仍为「基础尺寸」时调用）：将当前窗口矩形向外扩展留白，
    // 基础 Dock 条矩形保持当前窗口位置不变。RepositionDock 之后会按边重新计算。
    if (m_hwnd && m_baseW > 0 && m_baseH > 0) {
        RECT cur = {};
        GetWindowRect(m_hwnd, &cur);
        int wx = cur.left   - left;
        int wy = cur.top    - top;
        int ww = (cur.right - cur.left) + left + right;
        int wh = (cur.bottom - cur.top) + top  + bottom;
        SetWindowPos(m_hwnd, ZInsertAfter(), wx, wy, ww, wh,
                     SWP_NOACTIVATE | (m_visible ? SWP_SHOWWINDOW : 0));
        m_baseRect = cur;   // 扩展前的矩形即基础 Dock 条
    }
}

void WindowManager::Destroy() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    // 注意：不在此 UnregisterClassW。窗口类 WINDOW_CLASS 为进程级共享资源，
    // 多 Dock 实例（DockManager 每边一个 DockEngine）共用同一类名；若首个实例销毁时
    // 反注册类，会导致其余仍存活实例的窗口/消息失效。类随进程退出自然清理（可接受泄漏）。
    m_classRegistered = false;
}

void WindowManager::Show(bool visible) {
    if (m_hwnd) {
        ShowWindow(m_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        m_visible = visible;
    }
}

void WindowManager::SetCompositionOwner(bool dcomp) {
    // P0-5 / P0-7：声明窗口的合成路径归属。
    //   dcomp=true：DirectComposition 逐像素 alpha 合成（WS_EX_NOREDIRECTIONBITMAP），
    //     此时 SetWindowCompositionAttribute(Acrylic) 因无可重定向位图而无效，
    //     背景模糊/着色由 DComp 视觉（RenderManager 绘制底座条）自身提供 →
    //     ApplyBackgroundBlur 内据此禁用 Acrylic 分支。
    //   dcomp=false：GDI 回退路径，允许 Acrylic/Accent/DwmBlurBehind 兜底模糊。
    m_dcompOwned = dcomp;
}

void WindowManager::PositionAtBottomCenter(int width, int height) {
    if (!m_hwnd) return;
    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    int y = workArea.bottom - height;
    SetWindowPos(m_hwnd, ZInsertAfter(), x, y, width, height,
                 SWP_NOACTIVATE | (m_visible ? SWP_SHOWWINDOW : 0));
}

int WindowManager::ApplyBackgroundBlur(bool enable, unsigned int tintABGR) {
    m_blurMode = 0;
    if (!m_hwnd || !enable) return 0;

    // DComp 路径（P0-7）：Acrylic/Accent 模糊依赖窗口重定向位图，而
    // 普通（非 layered）窗口 + WS_EX_NOREDIRECTIONBITMAP + DComp 窗口没有该位图，
    // SetWindowCompositionAttribute 在此下无效（且历史上从未在 DComp 下被调用，见架构 Q2）。
    // 故 DComp 模式显式跳过 Acrylic 分支，背景模糊由 DComp 视觉自身提供（m_blurMode 保持 0）。
    // 仅 GDI 回退路径（m_dcompOwned=false）才走下方 Acrylic/Accent/DwmBlurBehind 兜底。
    if (m_dcompOwned) return 0;

    // 首选：SetWindowCompositionAttribute（Acrylic → Accent Blur 逐级降级）
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setWCA = user32
        ? reinterpret_cast<PFN_SetWindowCompositionAttribute>(
              (void*)GetProcAddress(user32, "SetWindowCompositionAttribute"))
        : nullptr;
    if (setWCA) {
        ACCENT_POLICY_T policy = {ACCENT_ENABLE_ACRYLICBLURBEHIND_E,
                                  2 /*ACCENT_FLAG_DRAW_ALL_BORDERS*/,
                                  tintABGR, 0};
        WCA_DATA_T data = {WCA_ACCENT_POLICY_V, &policy, sizeof(policy)};
        if (setWCA(m_hwnd, &data)) {
            m_blurMode = 1;
            return m_blurMode;
        }
        policy.AccentState   = ACCENT_ENABLE_BLURBEHIND_E;
        policy.GradientColor = 0;
        if (setWCA(m_hwnd, &data)) {
            m_blurMode = 2;
            return m_blurMode;
        }
    }

    // 兜底：DwmEnableBlurBehindWindow（Vista+ 公开 API）
    DWM_BLURBEHIND bb = {};
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = TRUE;
    if (SUCCEEDED(DwmEnableBlurBehindWindow(m_hwnd, &bb))) {
        m_blurMode = 3;
        return m_blurMode;
    }

    return 0;
}

bool WindowManager::ApplyRoundedCorners() {
    m_rounded = false;
    if (!m_hwnd) return false;
    // Win11：DWMWA_WINDOW_CORNER_PREFERENCE(33) = DWMWCP_ROUND(2)；Win10 调用失败即忽略
    const DWORD attrCornerPref = 33;
    DWORD pref = 2;
    if (SUCCEEDED(DwmSetWindowAttribute(m_hwnd, attrCornerPref,
                                        &pref, sizeof(pref)))) {
        m_rounded = true;
    }
    return m_rounded;
}

void WindowManager::RepositionBottomCenter(int width, int height, int monitorIndex) {
    if (!m_hwnd) return;

    RECT work = {};
    MonitorFindCtx ctx;
    ctx.target = (monitorIndex >= 0 && monitorIndex < GetMonitorCount())
                     ? monitorIndex : 0;
    EnumDisplayMonitors(nullptr, nullptr, FindMonitorProc,
                        reinterpret_cast<LPARAM>(&ctx));
    if (ctx.hmon) {
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(ctx.hmon, &mi)) work = mi.rcWork;
    }
    if (work.right - work.left <= 0) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);   // 兜底：主工作区
    }

    int x = work.left + ((work.right - work.left) - width) / 2;
    int y = work.bottom - height;
    SetWindowPos(m_hwnd, ZInsertAfter(), x, y, width, height,
                 SWP_NOACTIVATE | (m_visible ? SWP_SHOWWINDOW : 0));
}

// ═══ Step 10：通用停靠定位 + 位置微调 + Z 序 ═══
void WindowManager::RepositionDock(int width, int height, int monitorIndex,
                                   int edge, int edgeOffset, int centerOffset) {
    if (!m_hwnd) return;

    // 解析目标显示器工作区（同 RepositionBottomCenter）
    RECT work = {};
    MonitorFindCtx ctx;
    ctx.target = (monitorIndex >= 0 && monitorIndex < GetMonitorCount())
                     ? monitorIndex : 0;
    EnumDisplayMonitors(nullptr, nullptr, FindMonitorProc,
                        reinterpret_cast<LPARAM>(&ctx));
    if (ctx.hmon) {
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(ctx.hmon, &mi)) work = mi.rcWork;
    }
    if (work.right - work.left <= 0) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }

    // 沿停靠边居中 + centerOffset；垂直方向贴边 + edgeOffset（向屏幕内为正）
    int x = 0, y = 0;
    switch (edge) {
    case 1:   // top
        x = work.left + ((work.right - work.left) - width) / 2 + centerOffset;
        y = work.top + edgeOffset;
        break;
    case 2:   // left
        x = work.left + edgeOffset;
        y = work.top + ((work.bottom - work.top) - height) / 2 + centerOffset;
        break;
    case 3:   // right
        x = work.right - width - edgeOffset;
        y = work.top + ((work.bottom - work.top) - height) / 2 + centerOffset;
        break;
    default:  // 0 = bottom
        x = work.left + ((work.right - work.left) - width) / 2 + centerOffset;
        y = work.bottom - height - edgeOffset;
        break;
    }

    // 基础 Dock 条矩形（不含留白）；窗口在此基础上向外扩展边距，避免放大/tooltip 被裁切
    m_baseW = width; m_baseH = height;
    m_baseRect = { x, y, x + width, y + height };
    int wx = x - m_insetL;
    int wy = y - m_insetT;
    int ww = width  + m_insetL + m_insetR;
    int wh = height + m_insetT + m_insetB;
    SetWindowPos(m_hwnd, ZInsertAfter(), wx, wy, ww, wh,
                 SWP_NOACTIVATE | (m_visible ? SWP_SHOWWINDOW : 0));
}

HWND WindowManager::ZInsertAfter() const {
    if (m_zOrder > 0)  return HWND_TOPMOST;
    if (m_zOrder == 0) return HWND_NOTOPMOST;
    return HWND_BOTTOM;
}

void WindowManager::ApplyZOrder(int zOrder) {
    m_zOrder = (zOrder > 0) ? 1 : (zOrder == 0 ? 0 : -1);
    if (!m_hwnd) return;

    // TOPMOST 是扩展样式位，SetWindowPos 会同步维护 WS_EX_TOPMOST
    SetWindowPos(m_hwnd, ZInsertAfter(), 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

UINT WindowManager::GetDpi() const {
    if (!m_hwnd) return 96;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef UINT(WINAPI* PFN_GetDpiForWindow)(HWND);
    auto p = user32
        ? reinterpret_cast<PFN_GetDpiForWindow>(
              (void*)GetProcAddress(user32, "GetDpiForWindow"))
        : nullptr;
    UINT dpi = p ? p(m_hwnd) : 0;
    return dpi ? dpi : 96;
}

int WindowManager::GetMonitorCount() {
    int n = 0;
    EnumDisplayMonitors(nullptr, nullptr, CountMonitorsProc,
                        reinterpret_cast<LPARAM>(&n));
    return n > 0 ? n : 1;
}

RECT WindowManager::GetDockRect() const {
    // Step 12：返回「基础 Dock 条」矩形（窗口内缩四边留白），用于命中/感应区判定，
    // 使透明留白区不会误触发悬停放大。
    return m_baseRect;
}

RECT WindowManager::GetFullWindowRect() const {
    // Step 12 / #5/#7：返回「含放大/tooltip 留白的整窗」矩形（= 基础 Dock 条向外扩展四边留白）。
    // 用于 WM_NCHITTEST：交互态下整窗（含放大图标溢出的留白）视为可交互，避免悬停放大卡死。
    RECT r = m_baseRect;
    r.left   -= m_insetL;
    r.top    -= m_insetT;
    r.right  += m_insetR;
    r.bottom += m_insetB;
    return r;
}

RECT WindowManager::GetWorkArea() const {
    // P0-4：当前显示器工作区（排除任务栏），供 TickIdle 四角→IDLE 判定。
    // 无窗口（无头模拟）时回退主显示器工作区，使角格坐标仍有确定基准。
    HMONITOR hmon = m_hwnd ? MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONULL)
                           : nullptr;
    if (!hmon) hmon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (hmon) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hmon, &mi)) return mi.rcWork;
    }
    // 兜底：虚拟屏幕
    return { 0, 0, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN) };
}
