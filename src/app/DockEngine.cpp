// src/app/DockEngine.cpp
// T10 拆分后的「协调器（Coordinator）」：
//   - DockEngine 持有全部成员，生命周期/动画驱动/命中测试/消息循环骨架保留在此；
//   - 状态机 / 图标集 / 交互三类职责下沉到 DockStateMachine / IconSetManager / DockInteraction；
//   - 所有被移动的方法在此仅作 1-3 行薄转发（{ m_xxx->Method(...); }），零行为变更；
//   - 子模块经 m_owner（DockEngine*）+ friend 反查本类私有成员，行为逐字节一致。
#include "DockEngine.h"
#include "DockStateMachine.h"     // T10 子模块：状态机 / 自动隐藏看门狗 / 边切换
#include "IconSetManager.h"       // T10 子模块：图标集 / 持久化 / 弹簧目标 / 放置
#include "DockInteraction.h"      // T10 子模块：消息 / 鼠标 / 点击 / 菜单 / 托盘 / 诊断
#include "DockEngineInternal.h"   // StateName / PositionName / GetMonitorWorkRect（inline free）
#include <shlobj.h>   // IDropTarget / 文件拖放（Step 8）
#include <ole2.h>     // OleInitialize / OleUninitialize（STA，拖放必需）
#include <commdlg.h>  // GetOpenFileNameW（Step 13 添加应用对话框）
#include <cmath>   // std::isfinite / std::clamp
#include <cstdio>   // 调试日志 fopen_s
#include <cstdarg>  // va_list
#include <cstdlib>  // getenv
#include "../utils/PathUtil.h"

// ═══════════════════════════════════════════════════════════
// Step 8：文件拖放目标（IDropTarget）—— 拖拽文件/快捷方式到 Dock 即添加
// ═══════════════════════════════════════════════════════════
class DockDropTarget : public IDropTarget {
public:
    explicit DockDropTarget(DockEngine* engine) : m_engine(engine), m_ref(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IDropTarget || riid == IID_IUnknown) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP DragEnter(IDataObject* pdo, DWORD, POINTL ptl, DWORD* pe) override {
        if (pe) *pe = DROPEFFECT_COPY;
        m_engine->DebugLog(L"[DROP] DragEnter enter pdo=%p pt=(%d,%d)\n",
                           (void*)pdo, (int)ptl.x, (int)ptl.y);
        // 缓存拖入路径（DragOver 不带 IDataObject，需在此读取一次）
        FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM med = {};
        m_dragPaths.clear();
        if (SUCCEEDED(pdo->GetData(&fe, &med)) && med.hGlobal) {
            HDROP hdrop = (HDROP)med.hGlobal;
            UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t buf[MAX_PATH + 2] = {};
                if (DragQueryFileW(hdrop, i, buf, MAX_PATH + 1))
                    m_dragPaths.push_back(std::wstring(buf));
            }
            ReleaseStgMedium(&med);
        }
        POINT pt = { (LONG)ptl.x, (LONG)ptl.y };   // 屏幕坐标，与 ComputeDragInsertIndex 一致
        m_engine->DebugLog(L"[DROP] DragEnter paths=%d pdo=%p pt=(%d,%d)\n",
                           (int)m_dragPaths.size(), (void*)pdo, (int)pt.x, (int)pt.y);
        m_engine->BeginExternalDropPreview(m_dragPaths, pt);
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL ptl, DWORD* pe) override {
        if (pe) *pe = DROPEFFECT_COPY;
        m_engine->DebugLog(L"[DROP] DragOver pt=(%d,%d) active=%d\n",
                           (int)ptl.x, (int)ptl.y, (int)m_engine->ExternalDragPreviewActive());
        POINT pt = { (LONG)ptl.x, (LONG)ptl.y };
        m_engine->MoveExternalDropPreview(pt);   // 光标移动 → 预览占位随之让位
        return S_OK;
    }
    STDMETHODIMP DragLeave() override {
        m_engine->DebugLog(L"[DROP] DragLeave\n");
        m_engine->EndExternalDropPreview(false);  // 取消拖放 → 撤销预览（不落盘）
        m_dragPaths.clear();
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* pdo, DWORD, POINTL ptl, DWORD* pe) override {
        POINT pt = { (LONG)ptl.x, (LONG)ptl.y };
        m_engine->DebugLog(L"[DROP] Drop active=%d pt=(%d,%d)\n",
                           (int)m_engine->ExternalDragPreviewActive(), (int)pt.x, (int)pt.y);
        if (m_engine->ExternalDragPreviewActive()) {
            m_engine->MoveExternalDropPreview(pt);     // 以最终光标位兜底校准槽位
            m_engine->EndExternalDropPreview(true);    // 提交：占位转正 + 落盘
        } else {
            // 兜底：未经过预览（异常路径）→ 直接插入到光标位置
            FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM med = {};
            if (SUCCEEDED(pdo->GetData(&fe, &med)) && med.hGlobal) {
                HDROP hdrop = (HDROP)med.hGlobal;
                UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
                int insertAt = m_engine->ComputeDragInsertIndex(pt);
                for (UINT i = 0; i < n; ++i) {
                    wchar_t buf[MAX_PATH + 2] = {};
                    if (DragQueryFileW(hdrop, i, buf, MAX_PATH + 1)) {
                        m_engine->AddIconFromDrop(std::wstring(buf), insertAt);
                        ++insertAt;   // 多文件依次右移，保持拖入顺序
                    }
                }
                ReleaseStgMedium(&med);
            }
        }
        m_dragPaths.clear();
        if (pe) *pe = DROPEFFECT_COPY;
        return S_OK;
    }
private:
    DockEngine* m_engine;
    LONG m_ref;
    std::vector<std::wstring> m_dragPaths;   // DragEnter 缓存的拖入路径
};

DockEngine::DockEngine() {
    QueryPerformanceFrequency(&m_perfFrequency);
    // 调试日志开关：运行前设置 OPEN_DOCK_DEBUG=1 即开启（诊断拖放预览用，平时零开销）。
    m_dbg = (getenv("OPEN_DOCK_DEBUG") != nullptr);
}

DockEngine::~DockEngine() {
    Shutdown();   // 子系统释放（内部经薄转发调用子模块，须先于子模块析构）
    // ═══ T10 拆分：子模块生命周期随宿主结束而释放 ═══
    m_stateMachine.reset();
    m_iconSet.reset();
    m_interaction.reset();
}

// ═══════════════════════════════════════════════════════════
// 生命周期
// ═══════════════════════════════════════════════════════════
HRESULT DockEngine::InitializeFromFile(const std::string& configPath) {
    m_exeDir = PathUtil::GetExeDir();
    std::string resolved = PathUtil::ResolveConfigPath(configPath, m_exeDir);
    m_configPath = resolved;

    AppConfig cfg;
    m_configMgr = std::make_unique<ConfigManager>();
    m_configMgr->Load(resolved, cfg);   // 失败则用默认值（错误容忍）

    // 将相对图标路径解析为相对于 exe 目录的绝对路径（共享默认 + 每边独立集）
    auto resolve = [&](std::vector<IconEntry>& list) {
        for (auto& entry : list) {
            if (!entry.path.empty() && !PathUtil::IsAbsolutePath(entry.path)) {
                entry.path = m_exeDir + entry.path;
            }
            if (!entry.workingDir.empty() && !PathUtil::IsAbsolutePath(entry.workingDir)) {
                entry.workingDir = m_exeDir + entry.workingDir;
            }
        }
    };
    resolve(cfg.sharedIcons);
    for (int e = 0; e < 4; ++e) resolve(cfg.edgeIcons[e]);
    // #4：激活边图标集 = 当前停靠边独立集（缺省回退共享默认）
    cfg.icons = !cfg.edgeIcons[(int)cfg.dock.position].empty()
        ? cfg.edgeIcons[(int)cfg.dock.position] : cfg.sharedIcons;
    return Initialize(cfg);
}

HRESULT DockEngine::Initialize(const AppConfig& config) {
    m_appConfig = config;

    // 诊断横幅：仅在 OPEN_DOCK_DEBUG=1 时写出。用于确认「当前运行的二进制是否包含本会话的预览代码」。
    // 若设置环境变量后运行仍【完全没有】 debug_output/openDock.log，则证明跑的是旧预编译版（release/），与源码无关。
    DebugLog(L"[BOOT] openDock started, PREVIEW_DEBUG build, m_exeDir=%s\n", m_exeDir.c_str());

    // #3 多实例：若由 DockManager 注入共享图标存储，使本地镜像与共享存储一致
    if (m_sharedEdgeIcons) m_appConfig.edgeIcons = *m_sharedEdgeIcons;
    if (m_sharedSharedIcons) m_appConfig.sharedIcons = *m_sharedSharedIcons;
    if (!m_configMgr) m_configMgr = std::make_unique<ConfigManager>();
    // #4：若配置含 per-edge 图标集，则激活边使用其独立集；否则沿用 config.icons
    if (!m_appConfig.edgeIcons[(int)m_appConfig.dock.position].empty())
        m_appConfig.icons = m_appConfig.edgeIcons[(int)m_appConfig.dock.position];
    m_appConfig.dock.iconCount = (int)m_appConfig.icons.size();

    // COM 初始化为 STA 公寓（OLE 文件拖放 RegisterDragDrop 要求 STA）
    HRESULT hrCo = OleInitialize(nullptr);
    m_comInitialized = SUCCEEDED(hrCo);

    const DockConfig& dc = m_appConfig.dock;

    // 统一配置 C++ 模块：加载四边独立配置，并据当前边构造多态几何实例
    DockConfigStore::Load(m_configPath, m_edgeConfigs);
    m_geom = MakeGeometry(dc.position);
    m_geom->computeBarSize(dc.iconCount, dc.baseIconSize, dc.iconSpacing,
                           dc.dockPadding, m_dockWidth, m_dockHeight);

    m_springs      = std::make_unique<SpringSystem>();
    m_layout       = std::make_unique<LayoutEngine>();
    m_hitTest      = std::make_unique<HitTestEngine>();
    m_render       = std::make_unique<RenderManager>();
    m_window       = std::make_unique<WindowManager>();
    m_iconProvider = std::make_unique<IconProvider>();

    // ═══ T10 拆分：构造子模块（薄转发 + friend）═══
    // 子系统就绪后再构造；子模块经 m_owner 反查本类成员，且本类方法经子模块薄转发。
    m_stateMachine = std::make_unique<DockStateMachine>(this);
    m_iconSet      = std::make_unique<IconSetManager>(this);
    m_interaction  = std::make_unique<DockInteraction>(this);

    ReconcileSprings(m_appConfig.icons, 0.0f);
    m_restDirty = true;

    HRESULT hr = m_window->Create(GetModuleHandleW(nullptr),
                                  (int)m_dockWidth, (int)m_dockHeight,
                                  &DockEngine::StaticWndProc, this);
    DOCK_HR_CHECK(hr, "WindowManager::Create");

    m_window->ApplyZOrder(m_appConfig.zOrder);
    ApplyPlacement();

    // 开机自启的落地已收敛到 DockManager::Initialize 的 AutoStart::Reconcile
    //（唯一真源，含「注册表被外部改动 → 反向同步回配置」）。这里不再重复调用，
    // 否则会在启动时把配置值单向盖写注册表，掩盖外部改动。
    {
        bool rounded = m_window->ApplyRoundedCorners();
        (void)rounded;
    }

    // P0-7：声明合成路径归属 —— 非 --force-gdi 即 DirectComposition 路径
    if (m_window) m_window->SetCompositionOwner(!m_forceGdi);

    m_render->SetAppearance(m_appConfig.backgroundOpacity, m_appConfig.cornerRadius);
    m_render->SetTooltipEnabled(m_appConfig.tooltipEnabled);
    m_render->SetShadowEnabled(m_appConfig.shadowEnabled);
    m_render->SetBarVisible(m_appConfig.dockBarVisible);   // #N 底座背景条显隐（默认隐藏）
    m_render->SetForceGdiFallback(m_forceGdi);   // 测试钩子：--force-gdi 强制 GDI 回退
    // Step 12 / #6：渲染器首次烘焙前先同步内容留白(inset)，使 m_offsetX/Y 与 m_winW/m_winH 就绪
    {
        int il = 0, it = 0, ir = 0, ib = 0;
        ComputeInsets(il, it, ir, ib);
        m_render->SetContentInsets(il, it, ir, ib);   // 设定 m_offsetX/Y 与 m_winW/m_winH
    }
    hr = m_render->Initialize(RenderManager::Mode::Windowed, m_window->GetHwnd(), dc);
    DOCK_HR_CHECK(hr, "RenderManager::Initialize");

    // Step 12：RenderManager::Initialize 之后再次同步内容留白（m_dockWidth 此刻已就绪）
    {
        int il = 0, it = 0, ir = 0, ib = 0;
        ComputeInsets(il, it, ir, ib);
        m_render->SetContentInsets(il, it, ir, ib);
    }

    auto imgs = m_iconProvider->LoadIcons(m_appConfig);
    if (!imgs.empty()) m_render->LoadIconTextures(imgs);
    // 记录当前已加载位图对应的图标路径顺序（轻量重排复用，不解码）
    {
        std::vector<std::wstring> rpaths;
        rpaths.reserve(m_appConfig.icons.size());
        for (auto& e : m_appConfig.icons) rpaths.push_back(e.path);
        m_render->SetIconRenderPaths(rpaths);
    }
    // 缓存显示名（Tooltip 每帧使用，避免重复取）
    m_iconNames.resize(m_appConfig.icons.size());
    for (size_t i = 0; i < m_iconNames.size(); ++i)
        m_iconNames[i] = m_iconProvider->GetDisplayName(i);

    m_state = DockState::Hidden;

    m_autoHide    = m_appConfig.autoHide;
    m_showDelayMs = m_appConfig.showDelayMs;
    m_hideDelayMs = m_appConfig.hideDelayMs;
    m_mousePenetrating = false;
    m_showCountdown = 0.0f;
    m_hideCountdown = 0.0f;
    m_probeHoverPending = false;

    // Bugfix（用户报障 B「启动流程被误判成图标拖拽」）：启动链路显式落定非拖拽态。
    // 成员默认值本已如此，但这里必须是【显式后置条件】而非默认值的巧合 ——
    // Initialize 可在同一 DockEngine 实例上被重入调用（换边 / 重建），
    // 任何上一轮遗留的拖拽态都不允许穿越一次初始化活到新的一轮。
    m_dragging  = false;
    m_dragMoved = false;
    m_dragIndex = -1;
    m_dragStart = {};

    m_initialized = true;
    // #7：自动隐藏初始隐藏，启动看门狗持续探测边缘感应区
    if (m_autoHide) UpdateIdleWatchdog();
    return S_OK;
}

HRESULT DockEngine::Show() {
    if (!m_initialized) return E_NOT_VALID_STATE;
    if (m_state != DockState::Hidden) return S_FALSE;

    // 真实窗口模式：注册文件拖放（Step 8）。系统托盘图标由 DockManager 统一添加
    if (m_render && m_render->GetMode() == RenderManager::Mode::Windowed) {
        if (!m_dropTarget) m_dropTarget = new DockDropTarget(this);
        if (m_dropTarget && GetHwnd()) RegisterDragDrop(GetHwnd(), m_dropTarget.Get());
    }

    // Bugfix（用户报障 B）：弹出即清空拖拽态。Show() 只在 m_state == Hidden 时走到这里，
    // 而隐藏态下不可能存在合法的进行中拖拽（拖拽必须始于对可见图标的按下），
    // 因此这里的任何残留都是脏状态，必须在窗口重新可见之前清掉 ——
    // 否则它会紧接着被 TickIdle 的「已离开本边感应区 → 删除图标」分支消费。
    m_dragging  = false;
    m_dragMoved = false;
    m_dragIndex = -1;

    EnterState(DockState::Entering);
    m_entryReleased = 0;   // 入场级联从 0 开始，逐帧释放
    m_window->Show(true);
    m_showCountdown = 0.0f;
    m_hideCountdown = 0.0f;
    m_probeHoverPending = false;
    SetPenetration(false);   // 弹出即进入交互态（非穿透），待鼠标离开再转为空闲穿透
    StartAnimationLoop();
    return S_OK;
}

HRESULT DockEngine::Hide() {
    if (!m_initialized) return E_NOT_VALID_STATE;
    if (m_state == DockState::Hidden || m_state == DockState::Exiting) return S_FALSE;

    EnterState(DockState::Exiting);
    ApplyExitTargets();
    StartAnimationLoop();
    return S_OK;
}

void DockEngine::Shutdown() {
    if (!m_initialized) return;
    StopAnimationLoop();
    StopWatchdog();
    RemoveTrayIcon();
    if (m_dropTarget && GetHwnd()) RevokeDragDrop(GetHwnd());   // Step 8
    if (m_render) m_render->Shutdown();
    if (m_window) m_window->Destroy();
    m_initialized = false;
    if (m_comInitialized) { OleUninitialize(); m_comInitialized = false; }
}

// ═══════════════════════════════════════════════════════════
// 消息循环 / 定时器（Windowed）
// ═══════════════════════════════════════════════════════════
LRESULT CALLBACK DockEngine::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DockEngine* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<DockEngine*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DockEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->m_interaction->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ═══════════════════════════════════════════════════════════
// 动画驱动（STAY：核心算法零 OS 依赖、可无头）
// ═══════════════════════════════════════════════════════════
void DockEngine::OnAnimationTick(float dt) {
    if (!m_initialized) return;
    dt = std::clamp(dt, DockConstants::DT_MIN, DockConstants::DT_MAX);
    m_stateTime += dt;

    // 入场级联释放（每 30ms 释放一个图标的入场目标）
    if (m_state == DockState::Entering) {
        while (m_entryReleased < m_appConfig.dock.iconCount &&
               m_stateTime >= m_entryReleased * ENTRY_STAGGER_SEC) {
            ApplyEntryTargets(m_entryReleased);
            ++m_entryReleased;
        }
    }

    // 弹跳回落倒计时
    if (m_bounceResetTimer > 0.0f) {
        m_bounceResetTimer -= dt;
        if (m_bounceResetTimer <= 0.0f && m_bounceIconIndex >= 0
            && m_bounceIconIndex < (int)m_iconSprings.size()) {
            m_springs->SetTarget(m_iconSprings[(size_t)m_bounceIconIndex].offsetId, 0.0f);
            m_bounceResetTimer = -1.0f;
        }
    }

    // 物理积分（验收计时）
    LARGE_INTEGER pfFreq, pfA, pfB;
    QueryPerformanceFrequency(&pfFreq);
    QueryPerformanceCounter(&pfA);
    bool allSettled = m_springs->Update(dt);
    QueryPerformanceCounter(&pfB);
    m_perfSpringUs += (double)(pfB.QuadPart - pfA.QuadPart) * 1e6 / (double)pfFreq.QuadPart;

    // Step 7：推进自动隐藏/显示延迟倒计时（动画帧与看门狗共用，避免漏计）
    AdvanceAutoHide(dt);

    // 布局计算（验收计时）—— 主轴坐标（水平=X / 竖直=Y）
    float mouseXCentered = m_geom->screenToMainAxis(
        (float)m_lastMousePos.x - (m_window ? (float)m_window->GetDockRect().left : 0.0f),
        (float)m_lastMousePos.y - (m_window ? (float)m_window->GetDockRect().top : 0.0f),
        m_dockWidth, m_dockHeight);
    QueryPerformanceCounter(&pfA);
    // P0-3：从稳定绑定组装弹簧值视图向量（每帧 3×n 次取值，零分配）
    std::vector<SpringRead> springReads;
    springReads.reserve(m_iconSprings.size());
    for (size_t i = 0; i < m_iconSprings.size(); ++i) {
        SpringRead r;
        r.scale   = m_springs->GetValue(m_iconSprings[i].scaleId);
        r.offsetY = m_springs->GetValue(m_iconSprings[i].offsetId);
        r.opacity = m_springs->GetValue(m_iconSprings[i].opacityId);
        springReads.push_back(r);
    }
    m_layout->CalculateLayout(m_appConfig.dock, mouseXCentered, m_mouseInDock,
                              springReads, m_currentLayouts);
    QueryPerformanceCounter(&pfB);
    m_perfLayoutUs += (double)(pfB.QuadPart - pfA.QuadPart) * 1e6 / (double)pfFreq.QuadPart;
    m_perfFrames++;

    // Step 8：显示区内拖动图标调整顺序 → 被拖图标位置跟随光标实时更新；
    // 放大/缩小效果保持原有鱼眼逻辑（由 ApplyHoverTargets 按光标位置驱动）。
    if (m_dragging && m_dragMoved && m_dragIndex >= 0
        && m_dragIndex < (int)m_currentLayouts.size() && m_window) {
        RECT dr = m_window->GetDockRect();
        float winX = (float)(m_lastMousePos.x - (dr.left - m_insetL));
        float winY = (float)(m_lastMousePos.y - (dr.top  - m_insetT));
        float dlx = winX - (float)m_insetL;
        float dly = winY - (float)m_insetT;
        float mainX = 0.0f, cross = 0.0f;
        m_geom->inverseMap(dlx, dly, m_dockWidth, m_dockHeight,
                           m_appConfig.dock.baseIconSize, m_appConfig.dock.dockPadding,
                           mainX, cross);
        m_currentLayouts[m_dragIndex].x      = mainX;
        m_currentLayouts[m_dragIndex].y      = cross;
    }

    if (m_externalDragActive && m_externalDragFloat >= 0
        && m_externalDragFloat < (int)m_currentLayouts.size() && m_window) {
        RECT dr = m_window->GetDockRect();
        float winX = (float)(m_lastMousePos.x - (dr.left - m_insetL));
        float winY = (float)(m_lastMousePos.y - (dr.top  - m_insetT));
        float dlx = winX - (float)m_insetL;
        float dly = winY - (float)m_insetT;
        float mainX = 0.0f, cross = 0.0f;
        m_geom->inverseMap(dlx, dly, m_dockWidth, m_dockHeight,
                           m_appConfig.dock.baseIconSize, m_appConfig.dock.dockPadding,
                           mainX, cross);
        m_currentLayouts[m_externalDragFloat].x     = mainX;
        m_currentLayouts[m_externalDragFloat].y     = cross;
        // 不强制 scale：放大/缩小走原有鱼眼逻辑（MoveExternalDropPreview 显式 ApplyHoverTargets），
        // 使被拖占位图标在光标处的放大/悬停效果与原图标一致。
    }

    // 渲染（Windowed: 零重绘 Transform；Headless: 记录布局）
    m_render->UpdateVisualTransforms(m_currentLayouts);
    if (m_render) m_render->UpdateTooltip(m_hoveredIndex, m_currentLayouts, m_iconNames, dt);
    m_render->CommitFrame();

    // 状态迁移：需用“最新收敛”判定（AdvanceAutoHide 可能在本帧切换动画目标）
    allSettled = m_springs->AllSettled();
    UpdateStateMachine(allSettled);
}

void DockEngine::StartAnimationLoop() {
    if (m_isAnimating) return;
    m_isAnimating = true;
    QueryPerformanceCounter(&m_lastTickTime);
    // 定时器线程仅投递消息，动画在主线程执行（COM 单线程安全）
    CreateTimerQueueTimer(
        &m_timerHandle, nullptr,
        [](PVOID ctx, BOOLEAN) {
            auto* engine = static_cast<DockEngine*>(ctx);
            HWND hwnd = engine->GetHwnd();
            if (hwnd) PostMessageW(hwnd, WM_APP_TICK, 0, 0);
        },
        this, 0, DockConstants::TIMER_INTERVAL_MS, WT_EXECUTEDEFAULT);
}

void DockEngine::StopAnimationLoop() {
    if (!m_isAnimating) return;
    m_isAnimating = false;
    if (m_timerHandle) {
        DeleteTimerQueueTimer(nullptr, m_timerHandle, INVALID_HANDLE_VALUE);
        m_timerHandle = nullptr;
    }
}

bool DockEngine::AreSpringsFinite() const {
    if (!m_springs) return true;
    for (auto& [id, s] : m_springs->GetAllSprings()) {
        if (!std::isfinite(s.value) || !std::isfinite(s.velocity)) return false;
    }
    return true;
}

bool DockEngine::AreSpringsSettled() const {
    return m_springs ? m_springs->AllSettled() : true;
}

const char* DockEngine::GetStateName() const {
    return StateName(m_state);
}

void DockEngine::ResetPerfAccum() {
    m_perfSpringUs = 0.0;
    m_perfLayoutUs = 0.0;
    m_perfFrames   = 0;
    if (m_render) m_render->ResetPerfRenderUs();
}

void DockEngine::GetPerfAccum(double& springUs, double& layoutUs, long long& frames) const {
    springUs = m_perfSpringUs;
    layoutUs = m_perfLayoutUs;
    frames   = m_perfFrames;
}

double DockEngine::GetPerfRenderUs() const {
    return m_render ? m_render->GetPerfRenderUs() : 0.0;
}

int DockEngine::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

// P0-4 / ADR §1.4.5：角格边长 = 【本边感应带的法向厚度】（交叉轴厚度 C = b + 2p），
// 与 DockStateMachine::ComputeRevealZoneFor 的扩展量严格同源。
// 旧写法 min(dockWidth, dockHeight) 在长条 Dock 上碰巧等于 C，但语义是错的
//（图标很少时主轴反而更短，会取到主轴长度），未来改 ComputeBarSize 会静默漂移。
// 该值现在【只服务 reveal 抑制】—— HitTestAt 已不再引用角格（见下方 §1.4 说明）。
int DockEngine::ComputeCornerSize() const {
    return (int)(m_geom->isVertical() ? m_dockWidth : m_dockHeight);
}

// ═══════════════════════════════════════════════════════════
// 命中测试（WM_NCHITTEST 区域判定）—— ADR §1.4 + §1.5 重写
//
// 判定（只有两层，且与 hover 判定【同一个函数】）：
//   0) 不可见态（Hidden / Exiting）→ 整体穿透 HTTRANSPARENT
//   1) 可见态 → 命中域 = (dockRect ⊕ s) ∪ ⋃_i iconHitRect(i, s)，其余留白穿透
//
// 相对旧实现删除的三件东西及理由：
//   · 【删】四角 IsInCornerIdleZone 拦截（旧 layer-0）
//       角格语义分离（§1.4）：角格约束管的是「Dock 该不该【出现】」，
//       不该管「已经出现的 Dock 该不该【响应】」。旧代码让四角 88×88 变成死区，
//       Left/Right/Bottom 停靠时【末端图标整个落在角格里】→ 看得见点不开（缺陷 D2，
//       对应用户报障 2「左侧最下面的图标放大后打不开」）。
//       「四角不得唤出 Dock」的硬约束仍由 DockStateMachine::TickIdle 的
//       (inOwnReveal && !inCorner) 承担 —— 一个语义只存在于一个地方。
//       不可见态整体 return false，穿透比旧实现更彻底，左下开始菜单 /
//       右下显示桌面热区不会被遮挡。
//   · 【删】kEdgeMargin = 48 的单向条带（旧 layer-1）
//       第三个膨胀常数，与 SENSE_AREA_EXPAND_PX 语义重复且只朝内单向，
//       是四边不对称的来源之一。现由各向同性的 dockRect ⊕ s 覆盖。
//   · 【删】可见态整窗 fullWin 兜底（旧 layer-3）
//       §1.3 把 insetMain 从 48 提到 243 后，整窗兜底会变成一个巨大的全吞点击
//       矩形，严重遮挡下层窗口。包络变大与命中收紧必须同批落地（§1.5.3 强耦合）。
//       删掉之后留白区真正穿透，净遮挡面积反而比修复前更小。
//
// D3 收口：这里【复用 HandleMouseMove 用的同一个 HitTestEngine::Test 调用】，
// 同一个膨胀常量、同一份 iconHitRect。系统认为「能点」的集合与程序认为「命中」
// 的集合从此恒等，不再存在「系统说能点、程序说没命中」的一圈死区。
// ═══════════════════════════════════════════════════════════
bool DockEngine::HitTestAt(int x, int y) {
    POINT pt = { x, y };
    RECT dr = m_window ? m_window->GetDockRect() : RECT{};
    bool client = false;

    // 0) 不可见态整体穿透（§1.4.2：按「像素上是否已经画出来」归边 ——
    //    Entering 归可见（已在画），Exiting 归不可见（正在擦））
    const bool dockVisible = (m_state != DockState::Hidden) && (m_state != DockState::Exiting);
    if (!dockVisible) {
        return false;
    }

    // 1) 可见态：命中域 = (dockRect ⊕ s) ∪ ⋃_i iconHitRect(i, s)
    if (m_window && m_layout && m_hitTest) {
        // 当前布局为空时用【静息】布局兜底（确保图标矩形尺寸/位置正确）
        if (m_currentLayouts.empty()) m_currentLayouts = EnsureRestLayout();
        HitTestEngine::HitResult hit = m_hitTest->Test(
            pt, dr, m_currentLayouts, m_appConfig.dock.position,
            m_dockWidth, m_dockHeight, m_appConfig.dock.baseIconSize,
            m_appConfig.dock.dockPadding, (float)DockConstants::SENSE_AREA_EXPAND_PX);
        // isInDock == PtInRect(dockRect ⊕ s)；hoveredIndex >= 0 == 落在某个 iconHitRect 内
        client = hit.isInDock || hit.hoveredIndex >= 0;
    }

    return client;
}

// ═══════════════════════════════════════════════════════════
// T10 薄转发器：状态机（DockStateMachine）
// ═══════════════════════════════════════════════════════════
void DockEngine::EnterState(DockState next) { m_stateMachine->EnterState(next); }
void DockEngine::UpdateStateMachine(bool allSettled) { m_stateMachine->UpdateStateMachine(allSettled); }
void DockEngine::SetAutoHideEnabled(bool on) { m_stateMachine->SetAutoHideEnabled(on); }
void DockEngine::SetPenetration(bool penetrate) { m_stateMachine->SetPenetration(penetrate); }
// P0 遮挡挂起：与 SetAutoHideEnabled 同样的薄转发风格，实现体在状态机
void DockEngine::SetOccluded(bool on) { m_stateMachine->SetOccluded(on); }
void DockEngine::UpdateIdleWatchdog() { m_stateMachine->UpdateIdleWatchdog(); }
void DockEngine::StartWatchdog() { m_stateMachine->StartWatchdog(); }
void DockEngine::StopWatchdog() { m_stateMachine->StopWatchdog(); }
void DockEngine::TickIdle(float dt) { m_stateMachine->TickIdle(dt); }
void DockEngine::AdvanceAutoHide(float dt) { m_stateMachine->AdvanceAutoHide(dt); }
RECT DockEngine::ComputeRevealZone() const { return m_stateMachine->ComputeRevealZone(); }
RECT DockEngine::ComputeRevealZoneFor(DockPosition edge, RECT dockRect) const { return m_stateMachine->ComputeRevealZoneFor(edge, dockRect); }
// Bugfix 回归辅助：所有图标 scale 目标的最小值（1.0=正常大小，0=被缩到最小）。
float DockEngine::GetMinIconScaleTarget() const {
    float mn = 1.0f;
    for (size_t i = 0; i < m_iconSprings.size(); ++i) {
        float t = m_springs->GetTarget(m_iconSprings[i].scaleId);
        if (t < mn) mn = t;
    }
    return mn;
}

// Bugfix 回归辅助：以 TickIdle 完全相同的方式取【本边】感应区（dr=GetDockRect()，
// edge=m_appConfig.dock.position），保证测试观测到的矩形与运行时判定所用的一致。
RECT DockEngine::GetOwnRevealZoneForTest() const {
    RECT dr = m_window ? m_window->GetDockRect() : RECT{ 0, 0, 0, 0 };
    return ComputeRevealZoneFor(m_appConfig.dock.position, dr);
}
DockPosition DockEngine::NextEnabledEdge(DockPosition avoid) const { return m_stateMachine->NextEnabledEdge(avoid); }
bool& DockEngine::EdgeRef(DockPosition edge) { return m_stateMachine->EdgeRef(edge); }
bool DockEngine::IsEdgeEnabled(DockPosition edge) const { return m_stateMachine->IsEdgeEnabled(edge); }
void DockEngine::ApplyDockPosition(DockPosition pos) { m_stateMachine->ApplyDockPosition(pos); }

// ═══════════════════════════════════════════════════════════
// T10 薄转发器：图标集（IconSetManager）
// ═══════════════════════════════════════════════════════════
std::wstring DockEngine::GetResolvedLaunchTarget(int index) const { return m_iconSet->GetResolvedLaunchTarget(index); }
bool DockEngine::IsLaunchTargetValid(int index) const { return m_iconSet->IsLaunchTargetValid(index); }
bool DockEngine::LaunchIcon(int index) { return m_iconSet->LaunchIcon(index); }
int DockEngine::GetBlurMode() const { return m_iconSet->GetBlurMode(); }
bool DockEngine::IsWindowRounded() const { return m_iconSet->IsWindowRounded(); }
unsigned int DockEngine::GetWindowDpi() const { return m_iconSet->GetWindowDpi(); }
int DockEngine::GetMonitorCount() const { return m_iconSet->GetMonitorCount(); }
std::wstring DockEngine::GetIconPath(int index) const { return m_iconSet->GetIconPath(index); }
std::wstring DockEngine::GetIconName(int index) const { return m_iconSet->GetIconName(index); }
int DockEngine::GetIconTextureCount() const { return m_iconSet->GetIconTextureCount(); }
bool DockEngine::RemoveIcon(int index, bool persist) { return m_iconSet->RemoveIcon(index, persist); }
bool DockEngine::ReorderIcon(int from, int to, bool persist) { return m_iconSet->ReorderIcon(from, to, persist); }
void DockEngine::LiveDragReorder(const POINT& pt) {
    // Step 8 / #4：拖拽进行中（已越 4px 阈值）→ 实时重排预览。
    // 根据光标在 Dock 显示区内的位置，把被拖图标插入对应槽位；其余图标按新顺序由
    // CalculateLayout 即时重排（槽位位置是几何推导、非弹簧，故为实时位移）。落盘延后到
    // WM_LBUTTONUP；光标移出显示区则交给删除分支处理。
    if (m_dragIndex < 0 || m_dragIndex >= (int)m_appConfig.icons.size()) return;
    RECT dr = m_window ? m_window->GetDockRect() : RECT{};
    RECT within = dr;
    InflateRect(&within, 8, 8);   // 与 WM_LBUTTONUP 的 dropR 容差一致
    if (!PtInRect(&within, pt)) return;

    const int insert = ComputeDragInsertIndex(pt);
    // ComputeDragInsertIndex 可能把「自身槽位」判为 best（光标恰在被拖图标静息位附近），
    // 此时 insert==m_dragIndex 或 m_dragIndex+1，但净位置不变 —— 跳过以避免每次
    // WM_MOUSEMOVE 都触发一次无谓的整体重建（重载纹理 + 重启动画循环）。
    const int newIndex = (m_dragIndex < insert) ? insert - 1 : insert;
    if (newIndex == m_dragIndex) return;

    // 轻量重排：复用已解码位图，不重新解码 / 不重定位窗口 —— 消除拖拽过程闪烁
    m_iconSet->ReorderIconsDuringDrag(m_dragIndex, insert);   // persist=false：实时重排不落盘
    m_dragIndex = newIndex;
}
bool DockEngine::AddIcon(const std::wstring& path, const std::wstring& name, bool persist, int insertAt) { return m_iconSet->AddIcon(path, name, persist, insertAt); }
void DockEngine::PersistConfigTo(const std::string& path) const { m_iconSet->PersistConfigTo(path); }
void DockEngine::AddIconFromDrop(const std::wstring& path, int insertAt) { m_iconSet->AddIconFromDrop(path, insertAt); }

// ═══════════════════════════════════════════════════════════
// 外部文件拖放实时预览（IDropTarget 进行中）
// ═══════════════════════════════════════════════════════════
void DockEngine::BeginExternalDropPreview(const std::vector<std::wstring>& paths, POINT pt) {
    if (paths.empty()) return;
    if (m_externalDragActive) EndExternalDropPreview(false);   // 异常重入：先撤销旧预览
    m_lastMousePos = pt;
    int insert = ComputeDragInsertIndex(pt);
    auto& v = m_appConfig.icons;
    // 在光标槽位插入连续 count 个半透明占位（preview=true），其余图标由 CalculateLayout 即时让位。
    int count = 0;
    for (const auto& path : paths) {
        IconEntry e;
        e.path = path;
        e.name = PathUtil::DeriveDisplayName(path);
        if (!PathUtil::IsAbsolutePath(e.path)) e.path = m_exeDir + e.path;
        int at = insert + count;
        if (at < 0 || at > (int)v.size()) at = (int)v.size();
        e.preview = true;
        v.insert(v.begin() + at, e);
        ++count;
    }
    m_externalDragActive = true;
    m_externalDragStart  = insert;
    m_externalDragCount  = count;
    m_externalDragFloat  = insert;   // 浮动首个占位（跟随光标显示被拖图标）
    if (m_window && !m_window->IsVisible()) Show();   // 确保预览可见（autoHide 隐藏态兜底）
    SyncCurrentEdgeIcons();   // 同步内存（含预览），不落盘
    RebuildIcons(false);      // 重建布局/弹簧/纹理 + 重启动画循环；不落盘
    // 重建弹簧后显式驱动一次鱼眼放大，使初始光标处的被拖占位图标即与原图标悬停效果一致。
    {
        float mx = m_geom->screenToMainAxis(
            (float)pt.x - (m_window ? (float)m_window->GetDockRect().left : 0.0f),
            (float)pt.y - (m_window ? (float)m_window->GetDockRect().top : 0.0f),
            m_dockWidth, m_dockHeight);
        ApplyHoverTargets(mx);
    }
    DebugLog(L"[DROP] BeginPreview done paths=%d insert=%d iconsNow=%d float=%d visible=%d\n",
             (int)paths.size(), insert, (int)m_appConfig.icons.size(),
             m_externalDragFloat, m_window ? (int)m_window->IsVisible() : -1);
}

void DockEngine::MoveExternalDropPreview(POINT pt) {
    if (!m_externalDragActive || m_externalDragCount <= 0) return;
    m_lastMousePos = pt;   // 浮动 override 用最新光标位置
    // OLE 拖拽不经由 HandleMouseMove，故在此显式维持鱼眼放大：被拖占位图标随光标放大/悬停，
    // 与原图标悬停效果一致（ApplyHoverTargets 按光标主轴位置驱动全部图标 scale 目标）。
    {
        float mx = m_geom->screenToMainAxis(
            (float)pt.x - (m_window ? (float)m_window->GetDockRect().left : 0.0f),
            (float)pt.y - (m_window ? (float)m_window->GetDockRect().top : 0.0f),
            m_dockWidth, m_dockHeight);
        ApplyHoverTargets(mx);
    }
    int insert = ComputeDragInsertIndex(pt);
    if (insert == m_externalDragStart) return;   // 仍在当前间隙内，无需搬动
    auto& v = m_appConfig.icons;
    // 把预览块从旧位置搬到新位置（保持内部顺序）。erase 在 start 之后会使目标偏移左移 count。
    std::vector<IconEntry> block;
    block.reserve(m_externalDragCount);
    for (int i = 0; i < m_externalDragCount; ++i)
        block.push_back(v[m_externalDragStart + i]);
    v.erase(v.begin() + m_externalDragStart,
            v.begin() + m_externalDragStart + m_externalDragCount);
    int dest = insert;
    if (dest > m_externalDragStart) dest -= m_externalDragCount;   // erase 后下标修正
    if (dest < 0) dest = 0;
    if (dest > (int)v.size()) dest = (int)v.size();
    v.insert(v.begin() + dest, block.begin(), block.end());
    m_externalDragStart = dest;
    m_externalDragFloat = dest;
    SyncCurrentEdgeIcons();
    // 轻量重排（复用已解码位图，不重新解码图标 / 不重定位窗口）—— 消除外部拖入时
    // 占位让位造成的整组纹理重载闪烁（与内部拖动 LiveDragReorder 同一路径）。
    m_iconSet->RelayoutDuringDrag();
    DebugLog(L"[DROP] MovePreview insert=%d startNow=%d icons=%d\n",
             insert, m_externalDragStart, (int)m_appConfig.icons.size());
}

void DockEngine::EndExternalDropPreview(bool commit) {
    if (!m_externalDragActive) return;
    auto& v = m_appConfig.icons;
    if (commit) {
        // 占位转正：preview=false 保留路径/顺序，随后落盘。
        for (int i = 0; i < m_externalDragCount && m_externalDragStart + i < (int)v.size(); ++i)
            v[m_externalDragStart + i].preview = false;
        SyncCurrentEdgeIcons();
        RebuildIcons(true);    // 落盘
    } else {
        // 撤销：移除预览块恢复原布局（不落盘）。
        int cnt = std::min(m_externalDragCount, (int)v.size() - m_externalDragStart);
        if (cnt > 0) v.erase(v.begin() + m_externalDragStart,
                             v.begin() + m_externalDragStart + cnt);
        SyncCurrentEdgeIcons();
        RebuildIcons(false);
    }
    m_externalDragActive = false;
    m_externalDragStart  = -1;
    m_externalDragCount  = 0;
    m_externalDragFloat  = -1;
}
void DockEngine::ApplyEntryTargets(int iconIndex) { m_iconSet->ApplyEntryTargets(iconIndex); }

// ═══════════════════════════════════════════════════════════
// 调试日志（真机 GUI 缺陷取证；落盘 <exedir>/debug_output/openDock.log）
// ═══════════════════════════════════════════════════════════
void DockEngine::DebugLog(const wchar_t* fmt, ...) {
    if (!m_dbg) return;   // 仅 OPEN_DOCK_DEBUG=1 时写出，平时零开销
    // 目录优先取 exe 目录（GetModuleFileNameW，不依赖 m_exeDir）；取不到则回退 %TEMP%。
    std::wstring dir;
    wchar_t buf[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH)) {
        std::wstring p(buf);
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir = p.substr(0, slash + 1);
    }
    if (dir.empty()) {
        wchar_t tmp[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tmp)) dir = tmp;
    }
    if (dir.empty()) dir = L".";
    std::wstring outDir = dir + L"debug_output";
    CreateDirectoryW(outDir.c_str(), nullptr);
    std::wstring path = outDir + L"\\openDock.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    va_list ap; va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fflush(f); fclose(f);
}
void DockEngine::ApplyHoverTargets(float mouseXCentered) { m_iconSet->ApplyHoverTargets(mouseXCentered); }
void DockEngine::ApplyRestTargets() { m_iconSet->ApplyRestTargets(); }
bool DockEngine::AnyScaleElevated() const { return m_iconSet->AnyScaleElevated(); }
bool DockEngine::IsFisheyeEnabled() const { return m_iconSet->IsFisheyeEnabled(); }
void DockEngine::ApplyExitTargets() { m_iconSet->ApplyExitTargets(); }
void DockEngine::TriggerBounce(int iconIndex) { m_iconSet->TriggerBounce(iconIndex); }
const std::vector<IconLayout>& DockEngine::EnsureRestLayout() const { return m_iconSet->EnsureRestLayout(); }
void DockEngine::ReconcileSprings(const std::vector<IconEntry>& newIcons, float initScaleOpacity) { m_iconSet->ReconcileSprings(newIcons, initScaleOpacity); }
void DockEngine::RebuildIcons(bool persist) { m_iconSet->RebuildIcons(persist); }
void DockEngine::SyncCurrentEdgeIcons() { m_iconSet->SyncCurrentEdgeIcons(); }
void DockEngine::PersistConfig() const { m_iconSet->PersistConfig(); }
void DockEngine::SetConfigPath(const std::string& path) { m_iconSet->SetConfigPath(path); }
void DockEngine::SetDockBarVisible(bool visible) { m_iconSet->SetDockBarVisible(visible); }
void DockEngine::SetPersistCallback(std::function<void(const AppConfig&)> cb) { m_iconSet->SetPersistCallback(cb); }
void DockEngine::SetSharedIcons(std::shared_ptr<std::array<std::vector<IconEntry>, 4>> edges, std::shared_ptr<std::vector<IconEntry>> shared) { m_iconSet->SetSharedIcons(edges, shared); }
void DockEngine::ApplyPlacement() { m_iconSet->ApplyPlacement(); }
int DockEngine::GetWindowZOrder() const { return m_iconSet->GetWindowZOrder(); }
RECT DockEngine::GetDockScreenRect() const { return m_iconSet->GetDockScreenRect(); }
void DockEngine::SetPlacementOverride(int edgeOffset, int centerOffset, int zOrder) { m_iconSet->SetPlacementOverride(edgeOffset, centerOffset, zOrder); }
void DockEngine::SetDockPosition(DockPosition pos) { m_iconSet->SetDockPosition(pos); }
void DockEngine::SetEdgeEnabled(DockPosition edge, bool enabled) { m_iconSet->SetEdgeEnabled(edge, enabled); }
void DockEngine::SetIconSize(float size) { m_iconSet->SetIconSize(size); }
void DockEngine::SetBackgroundOpacity(float opacity) { m_iconSet->SetBackgroundOpacity(opacity); }
void DockEngine::SetZOrder(int zOrder) { m_iconSet->SetZOrder(zOrder); }
bool DockEngine::AddAppViaDialog() { return m_iconSet->AddAppViaDialog(); }
bool DockEngine::AddFolderViaDialog() { return m_iconSet->AddFolderViaDialog(); }
void DockEngine::ComputeInsets(int& left, int& top, int& right, int& bottom) const { m_iconSet->ComputeInsets(left, top, right, bottom); }
bool DockEngine::GetIconScreenCenter(int index, float& screenX, float& screenY) const { return m_iconSet->GetIconScreenCenter(index, screenX, screenY); }
bool DockEngine::GetIconCurrentScreenCenter(int index, float& screenX, float& screenY) const { return m_iconSet->GetIconCurrentScreenCenter(index, screenX, screenY); }
bool DockEngine::GetLayout(int index, float& mainX, float& crossY) const { return m_iconSet->GetLayout(index, mainX, crossY); }
int DockEngine::ComputeDragInsertIndex(POINT pt) { return m_iconSet->ComputeDragInsertIndex(pt); }

// ═══════════════════════════════════════════════════════════
// T10 薄转发器：交互（DockInteraction）
// ═══════════════════════════════════════════════════════════
LRESULT DockEngine::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) { return m_interaction->WndProc(hwnd, msg, wParam, lParam); }
void DockEngine::HandleMouseMove(int screenX, int screenY) { m_interaction->HandleMouseMove(screenX, screenY); }
void DockEngine::HandleMouseLeave() { m_interaction->HandleMouseLeave(); }
void DockEngine::HandleClick(int screenX, int screenY) { m_interaction->HandleClick(screenX, screenY); }
void DockEngine::HandleMenuCommand(int cmd) { m_interaction->HandleMenuCommand(cmd); }
void DockEngine::AddTrayIcon() { m_interaction->AddTrayIcon(); }
void DockEngine::RemoveTrayIcon() { m_interaction->RemoveTrayIcon(); }
