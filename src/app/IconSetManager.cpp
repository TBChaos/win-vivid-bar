// src/app/IconSetManager.cpp
// DockEngine 子模块：图标集增删改 / 持久化 / 弹簧目标 / 布局查询 / 放置与边配置 / 性能验收
// 方法体从原 DockEngine.cpp 拆分，零行为变更；类声明见 IconSetManager.h
#include "DockEngine.h"
#include "IconSetManager.h"
#include "DockEngineInternal.h"
#include "../core/DockGeometryLimits.h"   // INV-ENVELOPE 反解常量（ADR §1.3）
#include "../utils/PathUtil.h"
#include <shlobj.h>
#include <ole2.h>
#include <commdlg.h>
#include <cmath>

std::wstring IconSetManager::GetResolvedLaunchTarget(int index) const {
    if (index < 0 || index >= (int)m_owner->m_appConfig.icons.size()) return L"";
    return m_owner->m_appConfig.icons[index].path;   // 已在 InitializeFromFile 中解析为绝对路径
}

bool IconSetManager::IsLaunchTargetValid(int index) const {
    if (index < 0 || index >= (int)m_owner->m_appConfig.icons.size()) return false;
    return !m_owner->m_appConfig.icons[index].path.empty();
}

// 通过 ShellExecute 启动目标（.exe / 文件夹 / 快捷方式 / 带参数均可）。
// 注意：仅在真实窗口消息 WM_LBUTTONDOWN 路径调用；模拟路径（SimulateClick）不会触发，
// 以保证自动化测试不会意外拉起真实进程。
bool IconSetManager::LaunchIcon(int index) {
    if (index < 0 || index >= (int)m_owner->m_appConfig.icons.size()) return false;
    const IconEntry& e = m_owner->m_appConfig.icons[index];
    if (e.path.empty()) return false;


    // #2 增强：目录路径用 explorer 显式打开，确保文件夹点击必能打开
    DWORD attr = GetFileAttributesW(e.path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                    e.path.c_str(), nullptr, SW_SHOWNORMAL);
        return (reinterpret_cast<INT_PTR>(r) > 32);
    }

    HINSTANCE r = ShellExecuteW(
        nullptr,
        L"open",
        e.path.c_str(),
        e.args.empty()      ? nullptr : e.args.c_str(),
        e.workingDir.empty() ? nullptr : e.workingDir.c_str(),
        SW_SHOWNORMAL);
    return (reinterpret_cast<INT_PTR>(r) > 32);
}

int IconSetManager::GetBlurMode() const {
    return m_owner->m_window ? m_owner->m_window->GetBlurMode() : 0;
}

bool IconSetManager::IsWindowRounded() const {
    return m_owner->m_window ? m_owner->m_window->IsRounded() : false;
}

unsigned int IconSetManager::GetWindowDpi() const {
    return m_owner->m_window ? m_owner->m_window->GetDpi() : 96;
}

int IconSetManager::GetMonitorCount() const {
    return WindowManager::GetMonitorCount();
}

std::wstring IconSetManager::GetIconPath(int index) const {
    if (index < 0 || index >= (int)m_owner->m_appConfig.icons.size()) return L"";
    return m_owner->m_appConfig.icons[index].path;
}

std::wstring IconSetManager::GetIconName(int index) const {
    if (index < 0 || index >= (int)m_owner->m_appConfig.icons.size()) return L"";
    return m_owner->m_appConfig.icons[index].name;
}

int IconSetManager::GetIconTextureCount() const {
    return m_owner->m_render ? (int)m_owner->m_render->GetIconBitmapCount() : 0;
}

void IconSetManager::RebuildIcons(bool persist) {
    const int n = (int)m_owner->m_appConfig.icons.size();
    m_owner->m_appConfig.dock.iconCount = n;

    // 重新计算 Dock 逻辑尺寸（竖直朝向自动交换宽高）
    m_owner->m_geom->computeBarSize(n, m_owner->m_appConfig.dock.baseIconSize,
                                    m_owner->m_appConfig.dock.iconSpacing,
                                    m_owner->m_appConfig.dock.dockPadding,
                                    m_owner->m_dockWidth, m_owner->m_dockHeight);

    // 增量重建弹簧（P0-3）：仅对增删/重排的 Δ 图标增删弹簧节点，稳定图标保留动画状态。
    // 运行时增删/重排后保持图标可见（新增节点初值 SCALE/OPACITY=1），避免重建后图标瞬隐。
    m_owner->ReconcileSprings(m_owner->m_appConfig.icons, 1.0f);

    // 重新加载图标纹理与显示名
    auto imgs = m_owner->m_iconProvider->LoadIcons(m_owner->m_appConfig);
    if (m_owner->m_render) m_owner->m_render->RebuildIconSet(imgs);
    // 记录当前已加载位图对应的图标路径顺序（轻量重排复用，不解码）
    if (m_owner->m_render) {
        std::vector<std::wstring> paths;
        paths.reserve(m_owner->m_appConfig.icons.size());
        for (auto& e : m_owner->m_appConfig.icons) paths.push_back(e.path);
        m_owner->m_render->SetIconRenderPaths(paths);
    }
    m_owner->m_iconNames.resize(m_owner->m_appConfig.icons.size());
    for (size_t i = 0; i < m_owner->m_iconNames.size(); ++i)
        m_owner->m_iconNames[i] = m_owner->m_iconProvider->GetDisplayName(i);

    // 重新定位窗口（Step 10：按配置的停靠边/偏移/显示器统一定位）
    m_owner->ApplyPlacement();

    // 持久化（生产路径）：统一入口优先回调（四边独立存储互不覆盖）
    if (persist) m_owner->PersistConfig();

    // 关键修复：入场/悬停动画收敛后动画循环会停止（进入 IDLE 静默，CPU 0%）。
    // 此时新增/删除/重排/改大小仅更新了配置与渲染器图标集，但没有任何 WM_APP_TICK
    // 驱动重绘，画面停留在旧布局，表现为「添加无效」。这里重启动画循环，下一帧即
    // 提交新布局（弹簧已处于静止目标值，渲染一帧后会再次收敛停止，无额外开销）。
    if (m_owner->GetHwnd()) m_owner->StartAnimationLoop();

    // 立即重算静息布局：确保新增/删除/重排后图标即刻可命中（不依赖下一帧 tick），
    // 避免「新增图标点不到」在动画尚未 tick 时的观感。OnAnimationTick 每帧会再覆盖。
    // 几何/图标集已变 → EnsureRestLayout 内部重算并清脏，与原 rest 弹簧布局逐像素一致。
    m_owner->m_currentLayouts = m_owner->EnsureRestLayout();
}

bool IconSetManager::RemoveIcon(int index, bool persist) {
    if (index < 0 || index >= (int)m_owner->m_appConfig.icons.size()) return false;
    m_owner->m_appConfig.icons.erase(m_owner->m_appConfig.icons.begin() + index);
    m_owner->SyncCurrentEdgeIcons();   // #4/#3 同步当前边（含共享存储）
    m_owner->RebuildIcons(persist);
    return true;
}

bool IconSetManager::ReorderIcon(int from, int to, bool persist) {
    // 纯数组重排（可单测）；from/to 为原始数组下标语义，to==n 表示拖到末尾。
    if (!ReorderIconEntries(m_owner->m_appConfig.icons, from, to)) return false;
    m_owner->SyncCurrentEdgeIcons();   // #4/#3 同步当前边（含共享存储）
    m_owner->RebuildIcons(persist);
    return true;
}

bool IconSetManager::ReorderIconsDuringDrag(int from, int to) {
    // 拖拽过程轻量重排：仅数据重排 + 视觉轻量重排（复用已解码位图），不落盘、
    // 不重新解码、不重定位窗口 —— 消除拖拽过程中整组纹理重载 / 窗口重定位造成的闪烁。
    if (!ReorderIconEntries(m_owner->m_appConfig.icons, from, to)) return false;
    SyncCurrentEdgeIcons();
    RelayoutDuringDrag();
    return true;
}

void IconSetManager::RelayoutDuringDrag() {
    // 增量重建弹簧（按 path 复用，稳定图标保留动画状态）
    ReconcileSprings(m_owner->m_appConfig.icons, 1.0f);
    // 复用已解码位图按 path 轻量重排视觉树（不重新解码、不重定位窗口、不重建背景）
    std::vector<std::wstring> paths;
    paths.reserve(m_owner->m_appConfig.icons.size());
    for (auto& e : m_owner->m_appConfig.icons) paths.push_back(e.path);
    if (m_owner->m_render) m_owner->m_render->RelayoutIcons(paths);
    // 立即重算静息布局，确保下一帧 tick 前画面已反映新顺序
    m_owner->m_currentLayouts = EnsureRestLayout();
    if (m_owner->GetHwnd()) m_owner->StartAnimationLoop();
}

bool IconSetManager::AddIcon(const std::wstring& path, const std::wstring& name, bool persist, int insertAt) {
    if (path.empty()) return false;
    IconEntry e;
    e.path = path;
    // 未显式提供 name 时，统一走 PathUtil::DeriveDisplayName 派生显示名
    // （正确处理驱动器根/尾部分隔符/点开头/含点文件夹名等边界，避免把完整路径写进 name 字段）。
    e.name = name.empty() ? PathUtil::DeriveDisplayName(path) : name;
    if (!PathUtil::IsAbsolutePath(e.path)) e.path = m_owner->m_exeDir + e.path;
    auto& v = m_owner->m_appConfig.icons;
    if (insertAt < 0 || insertAt >= (int)v.size()) {
        v.push_back(e);                         // 默认：追加到末尾
    } else {
        v.insert(v.begin() + insertAt, e);      // 拖放插入到光标位置（可落在中间）
    }
    m_owner->SyncCurrentEdgeIcons();   // #4/#3 同步当前边（含共享存储）
    m_owner->RebuildIcons(persist);
    return true;
}

void IconSetManager::AddIconFromDrop(const std::wstring& path, int insertAt) {
    // 统一走 PathUtil::DeriveDisplayName（与 AddIcon 共用，边界语义一致）
    std::wstring name = PathUtil::DeriveDisplayName(path);
    m_owner->AddIcon(path, name, true, insertAt);
}

void IconSetManager::PersistConfigTo(const std::string& path) const {
    if (m_owner->m_configMgr) m_owner->m_configMgr->SaveConfig(m_owner->m_appConfig, path);
}

void IconSetManager::SetEdgeEnabled(DockPosition edge, bool enabled) {
    std::lock_guard<std::mutex> lk(m_owner->m_edgeMutex);   // P1-7：串行化边开关，杜绝旧/新引擎短暂同边
    bool& ref = m_owner->EdgeRef(edge);
    if (enabled) {
        ref = true;
        // 启用该边 → 立即把 dock 吸附到该边（成为新的 home）
        if (m_owner->m_appConfig.dock.position != edge) m_owner->ApplyDockPosition(edge);
    } else {
        ref = false;
        if (m_owner->m_appConfig.dock.position == edge) {
            // 当前 home 被禁用 → 找下一个启用的边吸附过去；若已无其它启用边，则允许全禁用
            // （dock 隐藏，不再强制"至少保留一条边"——需求1：四边独立控制，允许任意组合含全 false）。
            DockPosition next = m_owner->NextEnabledEdge(edge);
            if (next != edge) m_owner->ApplyDockPosition(next);
            else m_owner->Hide();   // 无其它启用边：0 边 → 隐藏，不回滚
        }
    }
    m_owner->PersistConfig();
    m_owner->StartAnimationLoop();
}

void IconSetManager::SetConfigPath(const std::string& path) {
    m_owner->m_configPath = path;
}

// #N：切换 Dock 底座背景条显隐（默认隐藏，仅浮出图标）

void IconSetManager::SetDockBarVisible(bool visible) {
    m_owner->m_appConfig.dockBarVisible = visible;
    if (m_owner->m_render) m_owner->m_render->SetBarVisible(visible);
    m_owner->PersistConfig();
    m_owner->StartAnimationLoop();
}

// 多实例（DockManager）持久化协作：设置统一合并落盘回调

void IconSetManager::SetPersistCallback(std::function<void(const AppConfig&)> cb) {
    m_owner->m_persistCb = std::move(cb);
}

// 多实例（DockManager）持久化协作：注入共享图标存储（所有边 + 共享默认）

void IconSetManager::SetSharedIcons(std::shared_ptr<std::array<std::vector<IconEntry>, 4>> edges,
                                     std::shared_ptr<std::vector<IconEntry>> shared) {
    m_owner->m_sharedEdgeIcons  = edges;
    m_owner->m_sharedSharedIcons = shared;
}

// #3/#4：将当前激活边的图标集同步回 edgeIcons[当前边]，并（若由 DockManager 注入）
// 同步进共享存储，确保每条边独立编排、互不覆盖（单引擎 SaveConfig 回退也安全）。
// 单引擎/旧路径（无共享存储）下，使顶层 sharedIcons 与当前激活集一致，保证 SaveConfig
// 序列化的 icons（=sharedIcons）反映最新图标（否则顶层 icons 陈旧，验证回读会错位）。

void IconSetManager::SyncCurrentEdgeIcons() {
    int pos = (int)m_owner->m_appConfig.dock.position;
    if (pos < 0 || pos > 3) pos = 0;
    m_owner->m_appConfig.edgeIcons[pos] = m_owner->m_appConfig.icons;
    if (m_owner->m_sharedEdgeIcons)  (*m_owner->m_sharedEdgeIcons)[pos] = m_owner->m_appConfig.icons;
    if (!m_owner->m_sharedSharedIcons) m_owner->m_appConfig.sharedIcons = m_owner->m_appConfig.icons;
}

// 统一持久化入口：优先走 DockManager 合并落盘回调（四边独立、互不覆盖），
// 否则回退单引擎直接写盘（无头/旧路径）。

void IconSetManager::PersistConfig() const {
    if (m_owner->m_persistCb) {
        m_owner->m_persistCb(m_owner->m_appConfig);
    } else if (!m_owner->m_configPath.empty() && m_owner->m_configMgr) {
        m_owner->m_configMgr->SaveConfig(m_owner->m_appConfig, m_owner->m_configPath);
    }
}

// ═══ Step 13：运行时设置（右键菜单/验证用）═══

void IconSetManager::SetIconSize(float size) {
    float s = std::max(24.0f, std::min(128.0f, size));
    m_owner->m_appConfig.dock.baseIconSize = s;
    // RebuildIcons 重新计算 Dock 尺寸 + ApplyPlacement(重定位/留白/重算纹理) + 持久化
    m_owner->RebuildIcons(true);
}

void IconSetManager::SetBackgroundOpacity(float opacity) {
    float v = std::max(0.0f, std::min(1.0f, opacity));
    m_owner->m_appConfig.backgroundOpacity = v;
    if (m_owner->m_render) m_owner->m_render->SetAppearance(v, m_owner->m_appConfig.cornerRadius);
    m_owner->PersistConfig();
}

// #5 图层位置：1=总在前面(TOPMOST) 0=正常 -1=总在后面(BOTTOM)

void IconSetManager::SetZOrder(int zOrder) {
    int z = (zOrder > 0) ? 1 : (zOrder < 0 ? -1 : 0);
    m_owner->SetPlacementOverride(m_owner->m_appConfig.edgeOffset,
                                  m_owner->m_appConfig.centerOffset, z);
    m_owner->PersistConfig();
    m_owner->StartAnimationLoop();
}

bool IconSetManager::AddAppViaDialog() {
    if (!m_owner->m_window || !m_owner->m_window->GetHwnd()) return false;
    wchar_t pathBuf[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = m_owner->m_window->GetHwnd();
    ofn.lpstrFile   = pathBuf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L"应用程序 (*.exe;*.lnk)\0*.exe;*.lnk\0所有文件 (*.*)\0*.*\0";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        m_owner->AddIcon(std::wstring(pathBuf), L"", true);
        return true;
    }
    return false;
}

// #2：文件夹选择对话框，把选中的文件夹加入 Dock（文件夹本身无扩展名，
// 无法用「添加应用」的文件对话框过滤，故单独提供）。

bool IconSetManager::AddFolderViaDialog() {
    if (!m_owner->m_window || !m_owner->m_window->GetHwnd()) return false;
    BROWSEINFOW bi = {};
    bi.hwndOwner = m_owner->m_window->GetHwnd();
    bi.lpszTitle = L"选择要添加到 Dock 的文件夹";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t pathBuf[MAX_PATH] = {};
    bool ok = false;
    if (SHGetPathFromIDListW(pidl, pathBuf)) {
        m_owner->AddIcon(std::wstring(pathBuf), L"", true);
        ok = true;
    }
    CoTaskMemFree(pidl);
    return ok;
}

void IconSetManager::ApplyPlacement() {
    if (!m_owner->m_window || !m_owner->m_window->GetHwnd()) return;
    // Step 12：先按停靠边与 maxScale 计算放大溢出留白，使窗口足够大以容纳
    // 最大放大图标与 tooltip，避免被裁切。
    int il = 0, it = 0, ir = 0, ib = 0;
    m_owner->ComputeInsets(il, it, ir, ib);
    m_owner->m_insetL = il; m_owner->m_insetT = it; m_owner->m_insetR = ir; m_owner->m_insetB = ib;
    m_owner->m_window->SetContentInsets(il, it, ir, ib);
    if (m_owner->m_render) m_owner->m_render->SetContentInsets(il, it, ir, ib);

    // DockPosition → WindowManager 边缘编码（0=bottom 1=top 2=left 3=right）
    int edge = 0;
    switch (m_owner->m_appConfig.dock.position) {
    case DockPosition::Top:    edge = 1; break;
    case DockPosition::Left:   edge = 2; break;
    case DockPosition::Right:  edge = 3; break;
    default:                   edge = 0; break;
    }
    m_owner->m_window->RepositionDock((int)m_owner->m_dockWidth, (int)m_owner->m_dockHeight,
                                       m_owner->m_appConfig.monitorIndex, edge,
                                       m_owner->m_appConfig.edgeOffset,
                                       m_owner->m_appConfig.centerOffset);

    // Step 12 / #6：背景条 DComp Surface 需按当前（含留白的）窗口尺寸(m_winW×m_winH)重建，
    // 并以 (m_offsetX,m_offsetY) 偏移绘制圆角条；否则 Surface 仅 Dock 尺寸会把条裁切错位
    // （表现为「左上角半透明蒙版」）。尺寸/停靠边变化（Step 13 大小设置、SetDockPosition）同理。
    if (m_owner->m_render) m_owner->m_render->SetAppearance(m_owner->m_appConfig.backgroundOpacity,
                                                            m_owner->m_appConfig.cornerRadius);
}

// ═══════════════════════════════════════════════════════════
// ComputeInsets —— 由包络不变量 INV-ENVELOPE 反解（ADR §1.3.7）
//
//   INV-ENVELOPE：∀i, iconHitRect(i) ⊆ FullWindowRect
//   即「凡是能被看到 / 能被点到的像素，都必须落在 OS 窗口矩形内」。
//   窗口之外的像素收不到 WM_NCHITTEST，是「图标看得见却点不到」的物理根因。
//
//   旧实现用 halfGrowth=(maxScale−1)·b/2 与 tooltipPad=b 两个拍脑袋常数，
//   在 b=64/maxScale=2.5/R=4 下主轴只给了 48px，而鱼眼整排最多外扩 ~199px
//   ⇒ 末端图标满放大时约 150px 落在窗口外 ⇒ 缺陷 D1（Left 最下 / Right 最右
//   / Bottom 两端图标点不开）。
//
//   现在改为公式反解：参数（maxScale/magnifyRadius/bounceAmplitude/b/p）
//   任意变化都自动正确，**禁止再手改常数**。系数见 DockGeometryLimits.h。
//
//   注意：本函数把窗口撑大（透明区），必须与 DockEngine::HitTestAt 的
//   「命中域 = ⋃ iconHitRect ∪ (dockRect⊕s)」同批次落地 —— 否则中间态会出现
//   一个巨大的全吞点击矩形，严重遮挡下层窗口（ADR §1.5.3 强耦合说明）。
// ═══════════════════════════════════════════════════════════
void IconSetManager::ComputeInsets(int& left, int& top, int& right, int& bottom) const {
    const DockConfig& dc = m_owner->m_appConfig.dock;
    const float b = dc.baseIconSize;
    const float p = dc.dockPadding;
    const float A = dc.bounceAmplitude;
    const float R = dc.magnifyRadius;
    const int   n = dc.iconCount;   // 与 ComputeBarSize 同源（DockEngine.cpp:140）
    const float s = (float)DockConstants::SENSE_AREA_EXPAND_PX;

    // §1.3.2 弹簧可达集上下界（与 DockGeometryLimits 同源）
    const float sigmaMax = 1.0f + DockLimits::kScaleOvershoot  * (dc.maxScale - 1.0f);
    const float betaMax  =        DockLimits::kBounceOvershoot * A;
    const float betaMin  =      - DockLimits::kBounceUndershoot * A;

    // §1.3.3 结论 A：鱼眼总增益闭式上界 Σ(σ_i−1) ≤ γ_up·(maxScale−1)·min(R, n)
    //（Hann 窗 COLA 引理；n ≥ 2R−1 后与图标数无关）
    const float sumMax = DockLimits::kScaleOvershoot * (dc.maxScale - 1.0f)
                       * std::min(R, (float)std::max(n, 0));

    // §1.3.4 主轴（两端严格对称 —— 需求 5 的四边中心对称由此保证）
    const float overflowMain = b * 0.5f * sumMax - p;
    const float tipSide      = DockLimits::kTooltipMaxW * 0.5f - b * sigmaMax * 0.5f;
    const int   insetMain    = (int)std::ceil(std::max(0.0f,
                                   overflowMain + std::max(s, tipSide)));

    // §1.3.5 交叉轴（内侧要容纳 放大 + bounce 过冲 + 感应膨胀 + tooltip 储备）
    const int insetIn  = (int)std::ceil(std::max(0.0f,
                              betaMax + b * (sigmaMax - 1.0f)
                            + s + DockLimits::kTooltipReservePx - p));
    // 外侧只需容纳 bounce 回弹（betaMin<0）与感应膨胀，扣掉本就存在的 padding
    const int insetOut = (int)std::ceil(std::max(0.0f, s - p - betaMin));

    left = top = right = bottom = 0;
    switch (dc.position) {
    case DockPosition::Bottom: left = right = insetMain; top    = insetIn; bottom = insetOut; break;
    case DockPosition::Top:    left = right = insetMain; bottom = insetIn; top    = insetOut; break;
    case DockPosition::Left:   top = bottom = insetMain; right  = insetIn; left   = insetOut; break;
    case DockPosition::Right:  top = bottom = insetMain; left   = insetIn; right  = insetOut; break;
    }
}

int IconSetManager::GetWindowZOrder() const {
    return m_owner->m_window ? m_owner->m_window->GetZOrder() : 1;
}

RECT IconSetManager::GetDockScreenRect() const {
    // Step 12：返回「基础 Dock 条」矩形（窗口内缩留白），用于命中/验收判定
    return m_owner->m_window ? m_owner->m_window->GetDockRect() : RECT{};
}

bool IconSetManager::GetIconScreenCenter(int index, float& screenX, float& screenY) const {
    if (index < 0 || index >= (int)m_owner->m_appConfig.icons.size()) return false;
    RECT dr = m_owner->m_window ? m_owner->m_window->GetDockRect() : RECT{};
    // P0-1/2：零弹簧静息布局缓存（EnsureRestLayout）替代临时 rest 弹簧系统 + CalculateLayout，
    // 与原行为逐像素一致，热路径从 O(n) 弹簧构造降为 O(1) 查表。
    const std::vector<IconLayout>& layouts = m_owner->EnsureRestLayout();
    if (index >= (int)layouts.size()) return false;
    float dcx = 0.0f, dcy = 0.0f;
    m_owner->m_geom->mapLayout(layouts[(size_t)index].x, layouts[(size_t)index].y,
                                m_owner->m_dockWidth, m_owner->m_dockHeight,
                                m_owner->m_appConfig.dock.baseIconSize,
                                m_owner->m_appConfig.dock.dockPadding, dcx, dcy);
    screenX = (float)dr.left + dcx;
    screenY = (float)dr.top  + dcy;
    return true;
}

bool IconSetManager::GetLayout(int index, float& mainX, float& crossY) const {
    if (index < 0 || index >= (int)m_owner->m_currentLayouts.size()) return false;
    mainX  = m_owner->m_currentLayouts[(size_t)index].x;
    crossY = m_owner->m_currentLayouts[(size_t)index].y;
    return true;
}

bool IconSetManager::GetIconCurrentScreenCenter(int index, float& screenX, float& screenY) const {
    if (index < 0 || index >= (int)m_owner->m_currentLayouts.size()) return false;
    RECT dr = m_owner->m_window ? m_owner->m_window->GetDockRect() : RECT{};
    float dcx = 0.0f, dcy = 0.0f;
    m_owner->m_geom->mapLayout(m_owner->m_currentLayouts[(size_t)index].x,
                                m_owner->m_currentLayouts[(size_t)index].y,
                                m_owner->m_dockWidth, m_owner->m_dockHeight,
                                m_owner->m_appConfig.dock.baseIconSize,
                                m_owner->m_appConfig.dock.dockPadding, dcx, dcy);
    screenX = (float)dr.left + dcx;
    screenY = (float)dr.top  + dcy;
    return true;
}

void IconSetManager::SetPlacementOverride(int edgeOffset, int centerOffset, int zOrder) {
    m_owner->m_appConfig.edgeOffset   = edgeOffset;
    m_owner->m_appConfig.centerOffset = centerOffset;
    m_owner->m_appConfig.zOrder       = zOrder;
    if (m_owner->m_window && m_owner->m_window->GetHwnd()) {
        m_owner->m_window->ApplyZOrder(zOrder);
        m_owner->ApplyPlacement();
    }
}

void IconSetManager::SetDockPosition(DockPosition pos) {
    std::lock_guard<std::mutex> lk(m_owner->m_edgeMutex);   // P1-7：换边串行化（受锁保护，杜绝短暂同边）
    m_owner->ApplyDockPosition(pos);
}

// ═══════════════════════════════════════════════════════════
// 弹簧目标设置
// ═══════════════════════════════════════════════════════════

void IconSetManager::ApplyEntryTargets(int i) {
    if (i < 0 || i >= (int)m_owner->m_iconSprings.size()) return;
    uint32_t sid = m_owner->m_iconSprings[(size_t)i].scaleId;
    uint32_t aid = m_owner->m_iconSprings[(size_t)i].opacityId;
    m_owner->m_springs->SetParams(sid, m_owner->m_appConfig.entryParams);
    m_owner->m_springs->SetParams(aid, m_owner->m_appConfig.entryParams);
    m_owner->m_springs->SetTarget(sid, 1.0f);
    m_owner->m_springs->SetTarget(aid, 1.0f);
}

void IconSetManager::ApplyHoverTargets(float mouseXCentered) {
    for (size_t i = 0; i < m_owner->m_iconSprings.size(); ++i) {
        float target = m_owner->m_layout->CalcTargetScale((int)i, mouseXCentered,
                                                           m_owner->m_appConfig.dock);
        uint32_t id = m_owner->m_iconSprings[i].scaleId;
        m_owner->m_springs->SetParams(id, m_owner->m_appConfig.hoverParams);
        m_owner->m_springs->SetTarget(id, target);
    }
}

void IconSetManager::ApplyRestTargets() {
    for (size_t i = 0; i < m_owner->m_iconSprings.size(); ++i) {
        uint32_t id = m_owner->m_iconSprings[i].scaleId;
        // #4：复位用独立「快速临界阻尼」弹簧，避免复用 hoverParams 导致的过长回弹拖尾
        m_owner->m_springs->SetParams(id, m_owner->m_appConfig.restParams);
        m_owner->m_springs->SetTarget(id, 1.0f);
    }
}

// #1 看门狗辅助：是否有任一图标的放大目标仍 > 1（即处于鱼眼放大态）

bool IconSetManager::AnyScaleElevated() const {
    for (size_t i = 0; i < m_owner->m_iconSprings.size(); ++i) {
        if (m_owner->m_springs->GetTarget(m_owner->m_iconSprings[i].scaleId) > 1.0001f)
            return true;
    }
    return false;
}

// #N：当前边（按 dock.position）是否启用鱼眼放大。关闭的边仅静态显示图标。

bool IconSetManager::IsFisheyeEnabled() const {
    // #N：当前边（按 dock.position）是否启用鱼眼放大，统一由四边配置 EdgeConfig 提供。
    // 默认 Top 及四边均未覆盖 fisheye 时恒为 true，与旧 per-edge fisheyeTop/Bottom/
    // Left/Right 默认行为一致。
    return m_owner->m_edgeConfigs[(int)m_owner->m_appConfig.dock.position].fisheye;
}

void IconSetManager::ApplyExitTargets() {
    for (size_t i = 0; i < m_owner->m_iconSprings.size(); ++i) {
        uint32_t sid = m_owner->m_iconSprings[i].scaleId;
        uint32_t aid = m_owner->m_iconSprings[i].opacityId;
        uint32_t oid = m_owner->m_iconSprings[i].offsetId;
        m_owner->m_springs->SetParams(sid, SpringParams::Exit());
        m_owner->m_springs->SetParams(aid, SpringParams::Exit());
        m_owner->m_springs->SetTarget(sid, 0.0f);
        m_owner->m_springs->SetTarget(aid, 0.0f);
        m_owner->m_springs->SetTarget(oid, 0.0f);
    }
}

void IconSetManager::TriggerBounce(int iconIndex) {
    m_owner->m_bounceIconIndex = iconIndex;
    if (iconIndex < 0 || iconIndex >= (int)m_owner->m_iconSprings.size()) return;
    uint32_t id = m_owner->m_iconSprings[(size_t)iconIndex].offsetId;
    m_owner->m_springs->SetParams(id, m_owner->m_appConfig.bounceParams);
    m_owner->m_springs->SetTarget(id, m_owner->m_appConfig.dock.bounceAmplitude);   // 向上弹起
    // 100ms 后目标回 0，弹簧自然回落（dt 倒计时，兼容模拟与真实定时器）
    m_owner->m_bounceResetTimer = DockEngine::BOUNCE_RESET_SEC;
    m_owner->StartAnimationLoop();
}

// ═══════════════════════════════════════════════════════════
// P0-1/2/3：静息布局脏标记缓存 + 稳定弹簧绑定增量重建
// ═══════════════════════════════════════════════════════════

const std::vector<IconLayout>& IconSetManager::EnsureRestLayout() const {
    if (m_owner->m_restDirty) {
        m_owner->m_layout->CalculateRestLayout(m_owner->m_appConfig.dock, m_owner->m_restLayouts);
        m_owner->m_restDirty = false;
    }
    return m_owner->m_restLayouts;
}

void IconSetManager::ReconcileSprings(const std::vector<IconEntry>& newIcons, float initScaleOpacity) {
    // 建立「图标 key(path) → 既有绑定」索引，便于复用（稳定图标）/ 摘除（删除图标）。
    std::unordered_map<std::wstring, IconSpringBinding> existing;
    for (const auto& b : m_owner->m_iconSprings) existing[b.key] = b;

    std::vector<IconSpringBinding> next;
    next.reserve(newIcons.size());
    for (const auto& e : newIcons) {
        auto it = existing.find(e.path);
        if (it != existing.end()) {
            next.push_back(it->second);   // 稳定图标：复用既有弹簧节点（保留 value/velocity）
            existing.erase(it);
        } else {
            IconSpringBinding b;
            b.key      = e.path;
            b.scaleId   = m_owner->m_springs->CreateSpring(initScaleOpacity,
                                                           m_owner->m_appConfig.entryParams);
            b.offsetId  = m_owner->m_springs->CreateSpring(0.0f,
                                                           m_owner->m_appConfig.bounceParams);
            b.opacityId = m_owner->m_springs->CreateSpring(initScaleOpacity,
                                                           m_owner->m_appConfig.entryParams);
            next.push_back(b);
        }
    }

    // 摘除已删除图标的 3 个弹簧节点（增量删除，不再整体销毁 SpringSystem）。
    for (const auto& kv : existing) {
        m_owner->m_springs->Remove(kv.second.scaleId);
        m_owner->m_springs->Remove(kv.second.offsetId);
        m_owner->m_springs->Remove(kv.second.opacityId);
    }

    m_owner->m_iconSprings = std::move(next);
    m_owner->m_restDirty  = true;   // 图标集/数量变化 → 静息布局失效
}

int IconSetManager::ComputeDragInsertIndex(POINT pt) {
    RECT dr = m_owner->m_window ? m_owner->m_window->GetDockRect() : RECT{};
    int n = (int)m_owner->m_appConfig.icons.size();
    if (n <= 1) return 0;
    float mouseMain = m_owner->m_geom->screenToMainAxis(
        (float)(pt.x - dr.left), (float)(pt.y - dr.top),
        m_owner->m_dockWidth, m_owner->m_dockHeight);
    std::vector<IconLayout> rest;
    m_owner->m_layout->CalculateLayout(m_owner->m_appConfig.dock, 0.0f, false,
                                       *m_owner->m_springs, rest);
    int best = 0; float bestD = 1e9f;
    for (int i = 0; i < n && i < (int)rest.size(); ++i) {
        float d = std::abs(rest[i].x - mouseMain);
        if (d < bestD) { bestD = d; best = i; }
    }
    int to = (mouseMain > rest[best].x) ? (best + 1) : best;
    if (to > n) to = n;   // 允许 n：释放点越过最后一个图标 → 拖到末尾（修复末端插入缺陷）
    return to;
}
