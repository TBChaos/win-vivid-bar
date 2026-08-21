// src/app/DockManager.cpp
// 多 Dock 编排器（#4 升级：四边同时显示）
#include "DockManager.h"
#include "DockEngine.h"      // 单条边 Dock 单元（已验证的悬停/放大/自动隐藏/拖放/菜单）
#include "IconProvider.h"    // 图标提取/解码（托盘图标改由 AddTrayIcon 从 exe 内嵌 MAINICON 加载）
#include "../utils/PathUtil.h"
#include "../platform/AutoStart.h"
#include <windows.h>
#include <shellapi.h>        // NOTIFYICONDATA / Shell_NotifyIcon（系统托盘）+ SHQueryUserNotificationState
#include <shlobj.h>          // SHBrowseForFolderW（文件夹选择）
#include <commdlg.h>         // OPENFILENAMEW（文件选择）
#include <dwmapi.h>          // DwmGetWindowAttribute / DWMWA_CLOAKED（P0 遮挡检测）
#include <string>
#include <vector>

// 本地路径工具已统一至 PathUtil（src/utils/PathUtil.h）。

// ───────────────────────── 构造 / 析构 ─────────────────────────
DockManager::DockManager() = default;
DockManager::~DockManager() { Shutdown(); }

// ───────────────────────── 初始化 ─────────────────────────
HRESULT DockManager::Initialize(const AppConfig& cfg) {
    m_cfg = cfg;
    // #3 独立存储：DockManager 持有权威共享图标存储，各引擎通过 shared_ptr 直接读写
    // 自己的槽位；单引擎 SaveConfig 写全量 config 会把它边陈旧副本覆盖，故改为统一回调落盘。
    m_sharedEdgeIcons   = std::make_shared<std::array<std::vector<IconEntry>, 4>>(m_cfg.edgeIcons);
    m_sharedSharedIcons = std::make_shared<std::vector<IconEntry>>(m_cfg.sharedIcons);
    // #N：四边独立控制 —— 不再强制"至少显示一条边"。edgeEnabled 数组允许任意组合
    // （含全 false：仅托盘、不创建任何 Dock 引擎、不崩溃）。仅对启用边创建引擎。
    DockPosition order[] = { DockPosition::Bottom, DockPosition::Top,
                             DockPosition::Left,  DockPosition::Right };
    for (DockPosition e : order) {
        if (IsEdgeEnabled(e)) {
            // 单边创建失败不阻断其余边（保持原有容错语义）。
            CreateEdgeEngine(e);
        }
    }
    // 需求 7：开机自启对齐 —— 全进程【只在这里做一次】。
    // 旧实现在 DockEngine::Initialize 里每条边各调一次 ApplyAutoStart，四边启用时对同一
    // 注册表值盲写 4 次，且是"配置 → 注册表"单向写：exe 被移动后注册表仍指旧路径，
    // 开机启动静默失败。现改为 Reconcile（ADR §3.3 真值表，含 stale 自愈）。
    {
        bool effective = m_cfg.autoStart;
        [[maybe_unused]] const bool wrote = AutoStart::Reconcile(m_cfg.autoStart, effective);
        // 注意：不把 effective 回写进 m_cfg.autoStart。配置是【用户意图】，注册表只是
        // 【生效状态】。唯一 effective != intent 的情形是 intent=true 而注册表不可用
        //（组策略/安全软件），此时把意图改成 false 会让用户下次打开菜单发现勾自己没了，
        // 更困惑；保留意图，等注册表恢复可写时下次启动自动生效。
    }
    m_initialized = true;
    return S_OK;
}

HRESULT DockManager::InitializeFromFile(const std::string& configPath) {
    m_cfgMgr = std::make_unique<ConfigManager>();
    std::wstring exeDir = PathUtil::GetExeDir();
    AppConfig cfg;
    m_cfgMgr->Load(PathUtil::ResolveConfigPath(configPath, exeDir), cfg);
    // 相对图标路径解析为 exe 目录绝对路径（共享默认 + 每边独立集）
    auto resolve = [&](std::vector<IconEntry>& list) {
        for (auto& entry : list) {
            if (!entry.path.empty() && !PathUtil::IsAbsolutePath(entry.path))
                entry.path = exeDir + entry.path;
            if (!entry.workingDir.empty() && !PathUtil::IsAbsolutePath(entry.workingDir))
                entry.workingDir = exeDir + entry.workingDir;
        }
    };
    resolve(cfg.sharedIcons);
    for (int e = 0; e < 4; ++e) resolve(cfg.edgeIcons[e]);
    m_configPath = PathUtil::ResolveConfigPath(configPath, exeDir);
    return Initialize(cfg);
}

// ───────────────────────── 按边创建 / 销毁引擎 ─────────────────────────
HRESULT DockManager::CreateEdgeEngine(DockPosition edge) {
    // 复制主配置（含 edgeIcons / 全局设置），仅把停靠边设为该边；
    // DockEngine::Initialize 会据此自动选用 edgeIcons[edge] 或 sharedIcons 回退。
    AppConfig ec = m_cfg;
    ec.dock.position = edge;
    auto eng = std::make_unique<DockEngine>();
    // --force-gdi 须早于 Initialize：引擎在 Initialize 内把该标志下发 RenderManager。
    eng->SetForceGdiFallback(m_forceGdi);
    HRESULT hr = eng->Initialize(ec);
    if (FAILED(hr)) return hr;
    eng->SetConfigPath(m_configPath);     // 与单 Dock 一致的持久化目标
    // #3 注入共享图标存储 + 统一落盘回调（四边独立、互不覆盖）
    eng->SetSharedIcons(m_sharedEdgeIcons, m_sharedSharedIcons);
    eng->SetPersistCallback([this](const AppConfig& c) { SaveConfigTo(c); });
    m_docks[(int)edge] = std::move(eng);
    return S_OK;
}

void DockManager::DestroyEdgeEngine(DockPosition edge) {
    if (m_docks[(int)edge]) {
        m_docks[(int)edge]->Shutdown();
        m_docks[(int)edge].reset();
    }
}

// #3 统一落盘入口：合并各引擎通过回调传来的配置写完整四边配置。
// 全局设置（底座显隐/外观/自动隐藏/图层等）取自发起保存的引擎（各引擎全局设置同源）；
// 图标集以共享存储为准（每条边独立槽位，互不覆盖），避免单引擎 SaveConfig 把它边陈旧副本写盘。
void DockManager::SaveConfigTo(const AppConfig& fromEngine) {
    m_cfg.dockBarVisible  = fromEngine.dockBarVisible;
    m_cfg.backgroundOpacity = fromEngine.backgroundOpacity;
    m_cfg.cornerRadius    = fromEngine.cornerRadius;
    m_cfg.shadowEnabled   = fromEngine.shadowEnabled;
    m_cfg.tooltipEnabled  = fromEngine.tooltipEnabled;
    m_cfg.autoHide        = fromEngine.autoHide;
    m_cfg.showDelayMs     = fromEngine.showDelayMs;
    m_cfg.hideDelayMs     = fromEngine.hideDelayMs;
    m_cfg.autoStart       = fromEngine.autoStart;
    m_cfg.zOrder          = fromEngine.zOrder;
    m_cfg.monitorIndex    = fromEngine.monitorIndex;
    m_cfg.centerOffset    = fromEngine.centerOffset;
    m_cfg.edgeOffset      = fromEngine.edgeOffset;
    m_cfg.dock.maxScale   = fromEngine.dock.maxScale;
    m_cfg.dock.baseIconSize = fromEngine.dock.baseIconSize;
    m_cfg.dock.iconSpacing  = fromEngine.dock.iconSpacing;
    m_cfg.dock.dockPadding  = fromEngine.dock.dockPadding;
    m_cfg.dock.edgeTop    = fromEngine.dock.edgeTop;
    m_cfg.dock.edgeBottom = fromEngine.dock.edgeBottom;
    m_cfg.dock.edgeLeft   = fromEngine.dock.edgeLeft;
    m_cfg.dock.edgeRight  = fromEngine.dock.edgeRight;
    // 权威四边启用开关镜像到 dock.edge*（向后兼容旧读取端）
    m_cfg.dock.edgeTop    = m_cfg.edgeEnabled[(int)DockPosition::Top];
    m_cfg.dock.edgeBottom = m_cfg.edgeEnabled[(int)DockPosition::Bottom];
    m_cfg.dock.edgeLeft   = m_cfg.edgeEnabled[(int)DockPosition::Left];
    m_cfg.dock.edgeRight  = m_cfg.edgeEnabled[(int)DockPosition::Right];
    m_cfg.dock.fisheyeTop    = fromEngine.dock.fisheyeTop;
    m_cfg.dock.fisheyeBottom = fromEngine.dock.fisheyeBottom;
    m_cfg.dock.fisheyeLeft   = fromEngine.dock.fisheyeLeft;
    m_cfg.dock.fisheyeRight  = fromEngine.dock.fisheyeRight;
    // 图标集以【各边活跃引擎的当前 icons】为准（见 RefreshIconsFromEngines 注释）
    RefreshIconsFromEngines();
    if (m_cfgMgr && !m_cfgMgr->SaveConfig(m_cfg, m_configPath)) {
        return;
    }
    m_cfgDirty = false;
    if (m_trayHwnd) KillTimer(m_trayHwnd, TID_CFG_DEBOUNCE);
}

// ───────────────────────── 配置持久化（需求 6）─────────────────────────
// 图标真源：每条边【活跃引擎的 m_appConfig.icons】。
// 为什么不能直接用 m_sharedSharedIcons / m_cfg.sharedIcons：
//   IconSetManager::SyncCurrentEdgeIcons 里有一句
//       if (!m_sharedSharedIcons) m_appConfig.sharedIcons = m_appConfig.icons;
//   多实例（DockManager 四边）路径下 m_sharedSharedIcons 非空，这句永不执行，
//   sharedIcons 永远停留在【加载时的初值】。旧 SaveConfig 顶层 "icons" 写的就是它，
//   于是运行期用户改的排列在顶层 icons 里完全看不到。
void DockManager::RefreshIconsFromEngines() {
    for (int e = 0; e < 4; ++e) {
        if (m_docks[e]) {
            const std::vector<IconEntry>& live = m_docks[e]->GetConfig().icons;
            m_cfg.edgeIcons[e] = live;
            if (m_sharedEdgeIcons) (*m_sharedEdgeIcons)[e] = live;
        } else if (m_sharedEdgeIcons) {
            m_cfg.edgeIcons[e] = (*m_sharedEdgeIcons)[e];   // 该边未启用：沿用共享存储既有值
        }
    }

    // 顶层 "icons"（sharedIcons）= home 边的当前集，作为"新启用边"的初始化模板。
    // 与单引擎路径（IconSetManager:249）的语义保持一致，不再是加载初值。
    const int home = (int)m_cfg.dock.position;
    if (home >= 0 && home < 4 && m_docks[home]) {
        m_cfg.sharedIcons = m_cfg.edgeIcons[home];
        if (m_sharedSharedIcons) *m_sharedSharedIcons = m_cfg.sharedIcons;
    } else if (m_sharedSharedIcons) {
        m_cfg.sharedIcons = *m_sharedSharedIcons;
    }
}

void DockManager::MarkConfigDirty() {
    m_cfgDirty = true;
    // 去抖：拖拽重排一次会连续触发几十次持久化请求，800ms 内合并为一次写盘
    if (m_trayHwnd) SetTimer(m_trayHwnd, TID_CFG_DEBOUNCE, CFG_DEBOUNCE_MS, nullptr);
}

bool DockManager::FlushConfig(bool force) {
    if (!force && !m_cfgDirty) return true;
    if (!m_cfgMgr || m_configPath.empty()) {
        return false;
    }
    RefreshIconsFromEngines();
    const bool ok = m_cfgMgr->SaveConfig(m_cfg, m_configPath);
    if (ok) {
        m_cfgDirty = false;
        if (m_trayHwnd) KillTimer(m_trayHwnd, TID_CFG_DEBOUNCE);
    } else {
    }
    return ok;
}

// ───────────────────────── 显示 / 隐藏 / 运行 ─────────────────────────
HRESULT DockManager::ShowAll() {
    // #7：自动隐藏模式下 dock 初始保持隐藏，由看门狗探测感应区后滑出；
    // 仅非自动隐藏（常显）模式启动时直接展示全部 dock。
    if (m_cfg.autoHide) return S_OK;
    for (int e = 0; e < 4; ++e)
        if (m_docks[e]) m_docks[e]->Show();
    return S_OK;
}
HRESULT DockManager::HideAll() {
    for (int e = 0; e < 4; ++e)
        if (m_docks[e]) m_docks[e]->Hide();
    return S_OK;
}

int DockManager::Run() {
    CreateTrayHost();
    AddTrayIcon();
    // P0 遮挡检测：必须在【本线程】（即下方跑 GetMessageW 的线程）注册 —— 
    // WINEVENT_OUTOFCONTEXT 的事件是投递到注册线程的消息队列的，注册错线程就永远收不到。
    // 同时它依赖 m_trayHwnd 承载去抖 / 兜底 SetTimer，故必须排在 CreateTrayHost 之后。
    InstallOcclusionHook();
    // 显示桌面 / Win+D / Win+M 兜底看门狗（多层防御第三层）：1s 低频轮询四边 IsIconic，
    // 覆盖消息拦截（WM_SYSCOMMAND/WM_SIZE）漏网的最坏情形。业务隐藏态（autoHide/Hidden）
    // 由 RestoreFromOsMinimize 内部的 IsVisible 守卫排除，绝不误唤出。
    if (m_trayHwnd)
        m_minimizeWatchdogTimer =
            SetTimer(m_trayHwnd, TID_MINIMIZE_WATCHDOG, MINIMIZE_WATCHDOG_MS, nullptr);
    // 单一消息循环：所有 Dock 实例的 WM_APP_TICK / 鼠标 / 拖放消息均由各自 WndProc 处理，
    // 托盘回调由本类隐藏宿主窗口的 WndProc 处理。
    // 防御：丢弃进入循环前可能残留的陈旧 WM_QUIT（例如切换 DockManager 前对单实例 engine 的
    // Shutdown→DestroyWindow 曾误入队）；退出仅响应本编排器自身（托盘「退出」）发起的 WM_QUIT。
    {
        MSG drain = {};
        while (PeekMessageW(&drain, nullptr, 0, 0, PM_REMOVE)) {
            if (drain.message == WM_QUIT) break;
        }
    }
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void DockManager::Shutdown() {
    // P0 遮挡检测：【第一件事】就摘钩子 + 关定时器。理由有二：
    //   1) 下面马上要 DestroyEdgeEngine，若此时还有 WinEvent 回调 / 兜底定时器到点触发
    //      RecomputeOcclusion，就会摸到正在析构或已析构的 m_docks[e]（悬垂访问）；
    //   2) KillTimer 依赖 m_trayHwnd，必须排在 DestroyWindow(m_trayHwnd) 之前。
    UninstallOcclusionHook();
    // 需求 6：退出兜底落盘。必须在销毁引擎【之前】做 —— 图标真源是各边引擎的当前 icons，
    // 引擎一销毁就取不到了。force=true：即使没有脏标记也写，覆盖"某条改动路径漏调
    // PersistConfig"的情况（旧实现完全依赖每次操作即时落盘，漏一处就丢一次）。
    if (m_initialized) FlushConfig(/*force=*/true);
    RemoveTrayIcon();
    for (int e = 0; e < 4; ++e) DestroyEdgeEngine((DockPosition)e);
    if (m_trayHwnd) { DestroyWindow(m_trayHwnd); m_trayHwnd = nullptr; }
    if (m_trayHostClass) {
        // 类随进程存活（同 WindowManager 考量），此处不反注册
        m_trayHostClass = false;
    }
    m_initialized = false;
}

// ───────────────────────── 运行时边开关（#3 多实例版）─────────────────────────
void DockManager::SetEdgeEnabled(DockPosition edge, bool enabled) {
    std::lock_guard<std::mutex> lk(m_edgeMutex);   // P1-7：边所有权锁，原子交接杜绝同边双引擎
    bool& ref = EdgeRef(edge);
    if (enabled) {
        if (!ref) {
            ref = true;
            CreateEdgeEngine(edge);
            if (m_docks[(int)edge]) m_docks[(int)edge]->Show();
        }
    } else {
        if (ref) {
            DestroyEdgeEngine(edge);
            ref = false;
        }
    }
    // 持久化边开关（#3：经统一落盘入口合并共享图标存储，避免覆盖各边已编辑的图标集）
    SaveConfigTo(m_cfg);
}

// 图层位置（Z 序）：1=总在前面 0=正常 -1=总在后面。写全局设置 + 即时生效 + 落盘。
void DockManager::SetZOrder(int z) {
    int zc = (z > 0) ? 1 : (z == 0 ? 0 : -1);
    m_cfg.zOrder = zc;
    for (int e = 0; e < 4; ++e)
        if (m_docks[e]) m_docks[e]->SetZOrder(zc);
    SaveConfigTo(m_cfg);          // 合并共享图标存储后统一落盘
}

// P1-7：换边原子交接。在边所有权锁保护下：先销毁 from 边引擎（Hide + 释放槽位），
// 再启用并创建 to 边引擎；确保旧/新引擎不会短暂同边，任意时刻同边仅一个活跃引擎。
void DockManager::MoveHomeEdge(DockPosition from, DockPosition to) {
    std::lock_guard<std::mutex> lk(m_edgeMutex);
    if (from == to) return;
    // 1) 先释放旧边：销毁引擎并清开关，确保槽位腾空
    DestroyEdgeEngine(from);
    EdgeRef(from) = false;
    // 2) 再准备新边：若 to 未启用则置为启用（权威开关镜像）
    if (!IsEdgeEnabled(to)) EdgeRef(to) = true;
    // 3) 若 to 边尚无引擎则创建并弹出（已存在则保留，避免无谓重建）
    if (!m_docks[(int)to]) {
        CreateEdgeEngine(to);
        if (m_docks[(int)to]) m_docks[(int)to]->Show();
    }
    SaveConfigTo(m_cfg);
}

size_t DockManager::EngineCount() const {
    size_t n = 0;
    for (int e = 0; e < 4; ++e) if (m_docks[e]) ++n;
    return n;
}
DockEngine* DockManager::GetEngine(DockPosition edge) const {
    return m_docks[(int)edge].get();
}

// ───────────────────────── 边开关引用 ─────────────────────────
bool DockManager::IsEdgeEnabled(DockPosition edge) const {
    // #N：以权威开关 edgeEnabled 数组为准（索引=DockPosition: Bottom/Top/Left/Right）
    return m_cfg.edgeEnabled[(int)edge];
}
bool& DockManager::EdgeRef(DockPosition edge) {
    return m_cfg.edgeEnabled[(int)edge];
}

// ═══════════════════════════════════════════════════════════════════════════
// P0 遮挡检测（事件驱动惰性重算）
// 方案：deliverables/gstack/plan-occlusion-idle-cpu-2026-08-06.md
// 目标：dock 被其它窗口完全盖住时，把「唯一常驻 CPU 源」—— 各边 100ms 看门狗轮询
//       （四边 ~40 次/秒唤醒）—— 降到 0，稳态只剩 GetMessageW 阻塞。
// 判定方向刻意保守：宁可漏判（维持现状轮询），不可误判（dock 假死唤不出）。
// 因此所有拿不准的窗口（shell 背板 / cloak / 点击穿透 / 取不到矩形）一律【跳过】。
// ═══════════════════════════════════════════════════════════════════════════

// WINEVENTPROC 没有 user-data 参数，只能借静态指针回到实例。
// 仅在消息泵线程 Install/Uninstall 时写；WINEVENT_OUTOFCONTEXT 的事件也是投递到
// 注册线程的消息队列后再派发的，回调与引擎状态同线程 → 全程无需加锁。
static DockManager* g_occlusionOwner = nullptr;

namespace {

// inner ⊆ outer（P0 快路径只认「单个上层窗口完全包含 footprint」；
// 「两个半屏窗口拼起来盖住」的多窗并集属 P1 的 HRGN + RGN_DIFF 区域相减）
bool RectContainsRect(const RECT& outer, const RECT& inner) {
    return outer.left  <= inner.left  && outer.top    <= inner.top &&
           outer.right >= inner.right && outer.bottom >= inner.bottom;
}

bool IsEmptyRect(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

// 跳过本进程窗口（四条 dock + 托盘宿主 + 可能的菜单/对话框）。
// 注意：不能只靠 WINEVENT_SKIPOWNPROCESS —— 那只过滤【事件源】，
// z 序遍历里照样会遇到自己的窗口。
bool IsOwnProcessWindow(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

// DWM cloak：UWP 挂起窗口 / 虚拟桌面上的其它桌面窗口，IsWindowVisible 为真但实际不可见。
// 单独不能作为遮挡判据（普通窗口覆盖不触发 cloak），只用来剔除「假可见」的覆盖者。
bool IsCloakedWindow(HWND w) {
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != FALSE;
    return false;   // 取不到属性 → 按未 cloak 处理（保守：不因此判遮挡，见下方调用点）
}

// shell 背板类窗口白名单式跳过。这是本文件最关键的一条防误判守卫：
// dock 默认 zOrder=-1（总在后面）→ SetWindowPos(HWND_BOTTOM) → 桌面 Progman/WorkerW
// 很可能排在 dock【上方】，而它们的矩形覆盖整屏。不跳过的话四条边会被永久判定为
// 「被遮挡」，看门狗永不启动 —— dock 直接变砖。任务栏同理（底边 reveal 带可能整条
// 落在任务栏矩形内）。
bool IsShellBackdropWindow(HWND w) {
    wchar_t cls[64] = {};
    if (GetClassNameW(w, cls, (int)(sizeof(cls) / sizeof(cls[0]))) <= 0) return true;
    static const wchar_t* kSkip[] = {
        L"Progman",                  // 桌面
        L"WorkerW",                  // 桌面壁纸层 / 图标宿主
        L"SHELLDLL_DefView",         // 桌面图标视图
        L"Shell_TrayWnd",            // 主任务栏
        L"Shell_SecondaryTrayWnd",   // 副屏任务栏
        L"NotifyIconOverflowWindow", // 托盘溢出面板
    };
    for (const wchar_t* s : kSkip)
        if (lstrcmpiW(cls, s) == 0) return true;
    return false;
}

// 显示桌面检测：前台窗口是桌面背板(WorkerW 壁纸层 / Progman 桌面 / GetShellWindow)时，
// Explorer「显示桌面 / Win+D」已把桌面窗口置前并盖住 dock。dock 从不进入最小化态，
// 无法用 IsIconic 判定，必须靠前台窗口类。
static bool IsDesktopWindow(HWND w) {
    if (!w) return false;
    if (w == GetShellWindow()) return true;            // Progman（经典桌面窗口）
    wchar_t cls[64] = {};
    if (GetClassNameW(w, cls, (int)(sizeof(cls) / sizeof(cls[0]))) > 0) {
        if (lstrcmpiW(cls, L"WorkerW") == 0 || lstrcmpiW(cls, L"Progman") == 0)
            return true;
    }
    return false;
}

// z 序遍历上限（纯防御）。z 序链理论上不成环，但外部注入型软件什么都干得出来；
// 且 P1-5 的区域相减每步都要建 / 减一个 HRGN，遍历失控的代价比 P0 快路径更高。
constexpr int kOcclusionWalkGuard = 512;

// ── P1-5：HRGN 的 RAII 包装 ─────────────────────────────────────────────
// 区域相减路径有多个早退分支（减空 / CombineRgn 失败 / GDI 资源不足 / guard 到顶），
// 手工 DeleteObject 只要漏掉一条就是 GDI 对象泄漏 —— 对 dock 这种常驻进程是致命的：
// 每来一批窗口事件就走一遍，泄漏线性累积直到撞上每进程 10000 句柄上限。
class ScopedRgn {
public:
    ScopedRgn() = default;
    explicit ScopedRgn(HRGN h) : m_h(h) {}
    ~ScopedRgn() { reset(nullptr); }
    ScopedRgn(const ScopedRgn&)            = delete;
    ScopedRgn& operator=(const ScopedRgn&) = delete;
    void reset(HRGN h) { if (m_h) DeleteObject(m_h); m_h = h; }
    HRGN get() const { return m_h; }
    explicit operator bool() const { return m_h != nullptr; }
private:
    HRGN m_h = nullptr;
};

// P2-8 / P2-9：取 footprint 所在【真实显示器】的工作区（排除任务栏）。
// 与 DockEngineInternal.h 的 GetMonitorWorkRect 用同一套 MONITOR_DEFAULTTONEAREST
// 语义（reveal 感应带也是按它算的），保证「全屏判定用的屏」与「感应带用的屏」
// 是同一块 —— 否则多屏下会出现「按 A 屏判遮挡、按 B 屏算感应带」的错配。
bool GetFootprintWorkArea(const RECT& foot, RECT& work) {
    HMONITOR hm = MonitorFromRect(&foot, MONITOR_DEFAULTTONEAREST);
    if (!hm) return false;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hm, &mi)) return false;
    work = mi.rcWork;
    return !IsEmptyRect(work);
}

// ── P2-8：全屏独占 / 演示模式判定，按 footprint 所在显示器收窄 ──────────
//
// P0 缺陷：SHQueryUserNotificationState 是【整机】粒度的，一旦命中就把四条边一起
// 判遮挡。多屏用户在 1 号屏全屏看视频 / 打游戏时，2 号屏上完全可见的 dock 也被
// 误挂起（看门狗停摆 → 那条边唤不出，得等窗口事件或 1.2s 兜底定时器才救回来）。
//
// 修法：整机信号只当【触发条件】，是否对本边生效再按 footprint 所在显示器收窄 ——
// 取前台窗口矩形，要求它同时覆盖「该屏 work area」与「本边 footprint」才算数。
// 前台全屏窗口在别的屏 → 两个包含判定都不成立 → 本边不挂起，缺陷即消。
//
// 关于方案里的 DXGI 兜底（可选加分项）：刻意【不实现，降级】。IDXGISwapChain 的
// 全屏标志只能由持有该 swapchain 的进程自己查，跨进程取不到；为它引入新依赖或
// 探测线程与「遮挡态 CPU 归零」的目标直接相悖。功能上也不依赖它：真实独占全屏下
// 游戏窗口仍在 z 序上方且矩形等于整屏，会被下面的「单窗完全包含」快路径直接命中。
bool IsFullscreenAppCoveringFootprint(const RECT& foot) {
    QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;   // 安全默认=不遮挡
    if (FAILED(SHQueryUserNotificationState(&quns))) return false;
    if (quns != QUNS_RUNNING_D3D_FULL_SCREEN &&
        quns != QUNS_BUSY &&
        quns != QUNS_PRESENTATION_MODE) return false;

    RECT work = {};
    if (!GetFootprintWorkArea(foot, work)) return false;   // 定位不到屏 → 保守不挂起

    // 归属判定的锚点：前台窗口。全屏独占 / 演示模式的发起者必然是前台窗口，
    // 它落在哪块屏，这条整机信号就只对哪块屏上的边生效。
    HWND fg = GetForegroundWindow();
    if (!fg)                    return false;   // 有全屏信号却无前台窗口：无从归属
    if (IsOwnProcessWindow(fg)) return false;   // 前台是我们自己（托盘菜单）→ 与遮挡无关
    if (IsCloakedWindow(fg))    return false;

    RECT fr = {};
    if (!GetWindowRect(fg, &fr)) return false;
    if (IsEmptyRect(fr))         return false;

    // 覆盖 work area = 该屏被全屏应用占满；再叠一层「包含 foot」，兜住 dock 被摆在
    // work area 之外的情形（负 edgeOffset / 压任务栏），此时不该被这条判定误伤。
    // 注意方向：这里【只会增加】遮挡判定，判严一点最坏是继续轮询（性能没优化到），
    // 判松了却会让那条边唤不出（功能故障），故一律取严。
    return RectContainsRect(fr, work) && RectContainsRect(fr, foot);
}

}   // namespace

bool DockManager::IsFootprintOccluded(HWND self, const RECT& foot) const {
    // 退化矩形（无头 / 尚未定位）：任何窗口都「包含」它，必须直接判不遮挡，否则秒变砖。
    if (IsEmptyRect(foot)) return false;

    // ① 最快早退 —— 全屏独占 / 演示模式。此时 z 序遍历往往看不到覆盖窗口
    //    （独占 D3D 直接接管输出），只能靠 shell 的通知状态。
    //    P2-8：已从 P0 的【整机粒度】收窄为【footprint 所在显示器】粒度，
    //    多屏下 1 号屏全屏不再误挂 2 号屏的 dock（判定细节见上方函数注释）。
    if (IsFullscreenAppCoveringFootprint(foot)) return true;

    if (!self) return false;

    // ②③ 单趟 z 序遍历（只向【上层】走：GW_HWNDPREV = 更靠前）同时承载两条判定：
    //
    //   ② 快路径（P0 保留）：任一上层窗口矩形【完全包含】footprint → 立即判遮挡。
    //      覆盖绝大多数「一个最大化窗口盖住 dock」的日常场景，命中时不创建任何 GDI 对象。
    //
    //   ③ 区域相减（P1-5 新增）：修掉快路径对「两个半屏窗口拼起来盖住 dock」这类
    //      多窗并集覆盖的漏判。以 footprint 建待覆盖区 acc，沿 z 序逐个减去上层窗口
    //      矩形（RGN_DIFF），acc 被减空即说明 footprint 已被并集完全盖住。
    //      acc 【懒创建】：只有真的走到需要相减的那一步才建，快路径命中时零 GDI 开销。
    //
    // 判定方向仍然保守（同 P0）：所有拿不准的窗口一律跳过，所有 GDI 失败一律按
    // 「不遮挡」返回 —— 漏判只是继续轮询（性能没优化到），误判会让 dock 变砖。
    ScopedRgn acc;
    int guard = 0;
    for (HWND w = GetWindow(self, GW_HWNDPREV);
         w != nullptr && guard < kOcclusionWalkGuard;
         w = GetWindow(w, GW_HWNDPREV), ++guard) {
        if (!IsWindowVisible(w)) continue;
        if (IsIconic(w))         continue;              // 最小化
        if (IsOwnProcessWindow(w)) continue;            // 自身四条 dock + 托盘宿主
        if (IsShellBackdropWindow(w)) continue;         // 桌面/任务栏背板（见上方注释）
        if (IsCloakedWindow(w))  continue;              // DWM cloak 的假可见窗口
        // 点击穿透型覆盖层（输入法候选框 / 截图遮罩 / 各类 HUD）通常视觉上也是透明的，
        // 按不遮挡处理，避免把 dock 误挂起。
        if (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TRANSPARENT) continue;

        RECT wr = {};
        if (!GetWindowRect(w, &wr)) continue;
        if (IsEmptyRect(wr))       continue;

        // ② 快路径：单窗完全包含（O(1) 早退）
        if (RectContainsRect(wr, foot)) return true;

        // ③ 区域相减
        RECT hit = {};
        if (!IntersectRect(&hit, &wr, &foot)) continue;   // 与 foot 不相交 → 对覆盖无贡献
        if (!acc) {
            acc.reset(CreateRectRgnIndirect(&foot));
            if (!acc) return false;                       // GDI 资源不足 → 保守判不遮挡
        }
        ScopedRgn wrgn(CreateRectRgnIndirect(&wr));
        if (!wrgn) return false;
        // CombineRgn 返回结果区域的类型：NULLREGION=空 / SIMPLEREGION / COMPLEXREGION，
        // 失败返回 ERROR。这里不写 `== ERROR`（该宏名过于常见，易被其它头文件重定义），
        // 改为白名单式判定：不是这三种合法类型就当失败处理。
        const int type = CombineRgn(acc.get(), acc.get(), wrgn.get(), RGN_DIFF);
        if (type == NULLREGION) return true;              // 减空 = 多窗并集完全盖住
        if (type != SIMPLEREGION && type != COMPLEXREGION) return false;   // 失败 → 保守
    }
    return false;
}

void DockManager::RecomputeOcclusion() {
    bool anyOccluded = false;
    for (int i = 0; i < 4; ++i) {
        DockEngine* e = m_docks[i].get();
        if (!e) continue;

        // (a) 拖拽中 / 鱼眼放大中：用户正在这条边上操作，绝不挂起
        //     （放大态还指望看门狗兜 WM_MOUSELEAVE 漏发，挂了会卡在放大态）。
        if (e->IsDragging() || e->IsAnyScaleElevated()) {
            e->SetOccluded(false);
            // 显示桌面激活时仍在操作：确保边已抬到桌面之上（解除遮挡隐藏后也需抬升）。
            if (m_showDesktopActive) ApplyEffectiveZOrder(e, true);
            continue;
        }
        HWND hwnd = e->GetHwnd();
        if (!hwnd) continue;

        // (b) 本边 footprint：
        //     - autoHide 隐藏态：窗口本身不可见，真正要问的是「唤出通路是否被盖住」，
        //       故用 reveal 感应带（与 TickIdle 判定同源，不另定义第二套感应区）；
        //     - 其余（可见 / 常显）：用含放大留白的整窗矩形。
        //     DockManager 是 DockEngine 的 friend，直接取私有成员，与 TickIdle 完全同源。
        RECT foot = {};
        if (e->m_window) {
            if (e->IsHidden() && e->IsAutoHideEnabled()) {
                foot = e->ComputeRevealZoneFor(e->m_appConfig.dock.position,
                                               e->m_window->GetDockRect());
            } else {
                foot = e->m_window->GetFullWindowRect();
            }
        }

        const bool wasOcclHid = e->DidOcclusionHideWindow();   // 解除遮挡前的隐藏态
        bool occ = IsFootprintOccluded(hwnd, foot);
        // 显示桌面激活时桌面窗口(WorkerW/Progman)盖住一切真实窗口，没有任何 dock 边
        // 还能被真实窗口遮挡；此处强制不判遮挡，避免屏幕顶边的 footprint 在显示桌面
        // 期间被时序竞态误置 m_occluded=true（→ autoHide 隐藏边看门狗 TickIdle 首行
        // 整体 return → hover 永远唤不出，见 ApplyShowDesktopState）。on 时统一清零。
        if (m_showDesktopActive) occ = false;
        e->SetOccluded(occ);      // 幂等：同值内部直接 return
        if (occ) {
            anyOccluded = true;
        } else if (m_showDesktopActive && wasOcclHid &&
                   !(e->IsHidden() && e->IsAutoHideEnabled())) {
            // 显示桌面激活期间，任何「从遮挡隐藏恢复为可见」的边都必须重新抬到桌面之上，
            // 否则它虽被状态机 Show(true) 但仍停在配置 Z 序(HWND_BOTTOM 等)，会被桌面窗口
            // 盖住 → 唤不出（典型即上边在显示桌面时被遮挡隐藏、解除时漏抬）。
            ApplyEffectiveZOrder(e, true);
        }
    }

    // 兜底定时器（防漏事件卡死）：仅在【存在被遮挡的边】时运行，全部解除即关。
    // 只做布尔重检，绝不 PostMessage 驱动动画 —— 遮挡期全局约 1 次/秒，
    // 相较原先四边 40 次/秒是两个数量级的下降。
    if (!m_trayHwnd) return;
    if (anyOccluded) {
        if (!m_occlusionFallbackTimer)
            m_occlusionFallbackTimer =
                SetTimer(m_trayHwnd, TID_OCCLUSION_FALLBACK, OCCLUSION_FALLBACK_MS, nullptr);
    } else if (m_occlusionFallbackTimer) {
        KillTimer(m_trayHwnd, TID_OCCLUSION_FALLBACK);
        m_occlusionFallbackTimer = 0;
    }
}

void DockManager::ScheduleOcclusionRecompute() {
    if (!m_trayHwnd) return;
    // 同 ID 重复 SetTimer = 重置计时，天然去抖：拖动窗口时 LOCATIONCHANGE 每帧一条，
    // 全部合并为「停手后 120ms 一次」重算。
    m_occlusionDebounceTimer =
        SetTimer(m_trayHwnd, TID_OCCLUSION_DEBOUNCE, OCCLUSION_DEBOUNCE_MS, nullptr);
}

void CALLBACK DockManager::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                        LONG idObject, LONG idChild, DWORD, DWORD) {
    // 只关心「顶层窗口本身」的事件。这条过滤是性能生命线：
    // EVENT_OBJECT_LOCATIONCHANGE 对 OBJID_CURSOR 会随鼠标移动每帧狂发。
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    switch (event) {
    case EVENT_SYSTEM_FOREGROUND:      // 前台切换：遮挡恢复主力信号 + 显示桌面检测
    case EVENT_SYSTEM_MINIMIZESTART:   // 最小化开始 / 结束
    case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_OBJECT_SHOW:            // 窗口显示 / 隐藏
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_LOCATIONCHANGE:  // 移动 / 缩放 / 最大化
    case EVENT_OBJECT_CLOAKED:         // 虚拟桌面切换 / UWP 挂起
    case EVENT_OBJECT_UNCLOAKED:
        break;
    default:
        return;
    }
    if (g_occlusionOwner) {
        g_occlusionOwner->ScheduleOcclusionRecompute();
        // 前台切换(含显示桌面把 WorkerW/Progman 置前)同时驱动显示桌面检测：
        // 置前的是桌面窗口 → 把 dock 抬到桌面之上，避免被「盖住」而失效。
        if (event == EVENT_SYSTEM_FOREGROUND)
            g_occlusionOwner->OnForegroundWindowChanged(hwnd);
    }
}

// 前台窗口切换 → 显示桌面检测。前台是桌面背板(WorkerW/Progman)即认为「显示桌面」激活。
// 仅状态翻转时向托盘窗口投递 WM_APP_SHOWDESKTOP，由 TrayWndProc 在消息循环里真正改 Z 序，
// 避免在 WinEvent 回调内直接做窗口操作引发自激/重入。
void DockManager::OnForegroundWindowChanged(HWND fg) {
    if (!fg) fg = GetForegroundWindow();
    const bool desktop = IsDesktopWindow(fg);
    if (desktop) m_desktopHwnd = fg;   // 记住当前置前的桌面窗口，用于精准插入其上方
    if (desktop != m_showDesktopActive) {
        m_showDesktopActive = desktop;
        if (m_trayHwnd)
            PostMessageW(m_trayHwnd, WM_APP_SHOWDESKTOP,
                         desktop ? (WPARAM)1 : (WPARAM)0, 0);
    }
}

// 应用【有效 Z 序】（显示桌面场景专用，全程 SWP_NOACTIVATE 不抢焦点）：
// - desktop=true（含 autoHide 隐藏态）：把所有边统一抬到 HWND_TOPMOST。
//     · 旧写法曾用 SetWindowPos(h, 桌面窗口) 把 dock 放到桌面窗口【之下】，结果被
//       WorkerW/Progman 整屏盖住 → 显示桌面期间完全不可见（日志 pos=3 below=Progman 实证）。
//       显示桌面时所有普通应用已被最小化，不存在「盖住应用」问题，故统一置顶最稳妥。
//     · autoHide 隐藏态同样抬升（只改 Z 序、不 ShowWindow），否则 reveal 弹出的窗口会落在
//       WorkerW 之下、看不到也点不到（这是「显示桌面后四边全唤不出」的根因）。
// - desktop=false：按配置 m_zOrder 正常落位（HWND_BOTTOM/NOTOPMOST/TOPMOST）。
void DockManager::ApplyEffectiveZOrder(DockEngine* e, bool desktop) {
    if (!e) return;
    WindowManager* wm = e->m_window.get();
    if (!wm) return;
    HWND h = wm->GetHwnd();
    if (!h) return;
    if (desktop) {
        // 显示桌面激活：把本边抬到桌面【之上】才可见。
        // 关键修正：旧写法 SetWindowPos(h, desk=WorkerW/Progman) 把 dock 放到桌面窗口
        // 【之下】(hWndInsertAfter=桌面窗口 = 置于其下方)，等于压在桌面背后 → 显示桌面期间
        // 完全不可见。显示桌面时所有普通应用已被最小化，不存在「盖住应用」问题，故统一用
        // HWND_TOPMOST 把 dock 置于最前，保证可见。退出显示桌面(desktop=false)时 ApplyZOrder
        // 按配置复原(bottom→HWND_BOTTOM)，「总在后面」语义不丢。autoHide 隐藏边同理抬升、不弹窗。
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        wm->ApplyZOrder(wm->GetZOrder());          // 内部 SWP_NOACTIVATE
    }
}

// 应用显示桌面状态到四条 dock 边：
// - desktop=true：桌面窗口(WorkerW/Progman)被置前并盖住 dock → 把每条【非 autoHide 隐藏态】的边
//   抬到桌面之上（SWP_NOACTIVATE 不抢焦点）。
//     · 业务隐藏态(autoHide)：用户未悬停边缘，绝不主动唤出（留交 reveal 逻辑）。
//     · 被遮挡隐藏态(m_occlusionHidWindow，IsVisible()==false)：显示桌面意味着已无真实窗口遮挡，
//       交给状态机正规解除 SetOccluded(false)（仅它会 Show(true) 且清 m_occlusionHidWindow），
//       解除后再抬 Z 序到桌面之上 —— 修复此前「上边被遮挡隐藏后永不抬升、仍被桌面盖住」的缺陷。
//     · 万一某边被 OS 最小化(IsIconic)也先拉起。
// - desktop=false：解除 → 恢复配置 Z 序（bottom/normal/topmost 按配置，全程不抢焦点）。
void DockManager::ApplyShowDesktopState(bool desktop) {
    for (int i = 0; i < 4; ++i) {
        DockEngine* e = m_docks[i].get();
        if (!e) continue;
        WindowManager* wm = e->m_window.get();
        if (!wm) continue;
        HWND h = wm->GetHwnd();
        if (!h) continue;
        // 显示桌面激活：桌面窗口(WorkerW/Progman)盖住一切真实窗口，此时任何 dock 都不该处于
        // 「被遮挡」态。必须清掉【所有边】的 m_occluded（含 autoHide 隐藏边）—— 否则 autoHide
        // 隐藏边会被卡死：DockStateMachine::TickIdle 首行 `if(m_occluded) return` 整体不执行，
        // hover 永远唤不出（典型即上边 footprint 在屏顶、静止时被某真实窗口盖住 → m_occluded
        // 在显示桌面【前】就被置真；显示桌面后桌面盖住真窗、本该清标记让看门狗重新 hover 唤出，
        // 却因下方 autoHide 隐藏态的 continue 跳过、标记永不清除 → 上边唤不出）。
        // 清遮挡不会误弹 autoHide 边：SetOccluded(false) 仅当 m_occlusionHidWindow 为真才
        // Show(true)；autoHide 自身隐藏态该标志恒 false，故只清标记、不弹窗，看门狗照常
        // 探测感应区把它唤出。解除显示桌面(desktop=false)时不在此清，留交 RecomputeOcclusion 重算。
        if (desktop) e->SetOccluded(false);
        // autoHide 隐藏态：显示桌面时仍【不主动弹窗】（不 ShowWindow，hover 才 reveal），
        // 但同样抬到桌面之上 —— 否则 reveal 弹出的窗口会落在 WorkerW 之下、看不到也点不到。
        // 下方 ShowWindow 对 autoHide 隐藏态是 no-op（IsIconic 恒 false），不会误弹。
        if (IsIconic(h)) ShowWindow(h, SW_SHOWNOACTIVATE);
        ApplyEffectiveZOrder(e, desktop);
    }
}

void DockManager::InstallOcclusionHook() {
    if (!m_trayHwnd) return;             // 需要宿主窗口承载去抖 / 兜底 SetTimer
    if (g_occlusionOwner == this) return;
    g_occlusionOwner = this;

    // 事件区间刻意拆窄（详见 DockManager.h 的注释）：一个 0x0003~0x8018 的大区间会把
    // NAMECHANGE / VALUECHANGE / STATECHANGE 等高频无关事件全部订阅进来。
    struct Range { DWORD lo, hi; };
    static const Range kRanges[OCCLUSION_HOOK_COUNT] = {
        { EVENT_SYSTEM_FOREGROUND,     EVENT_SYSTEM_FOREGROUND    },
        { EVENT_SYSTEM_MINIMIZESTART,  EVENT_SYSTEM_MINIMIZEEND   },
        { EVENT_OBJECT_SHOW,           EVENT_OBJECT_HIDE          },
        { EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
        { EVENT_OBJECT_CLOAKED,        EVENT_OBJECT_UNCLOAKED     },
    };
    int installed = 0;
    for (int i = 0; i < OCCLUSION_HOOK_COUNT; ++i) {
        // WINEVENT_OUTOFCONTEXT：不注入 DLL，事件异步投递到【本线程】消息队列；
        // 必须在跑 GetMessageW 的那个线程注册（本函数由 Run() 调用），否则收不到回调。
        m_winEventHooks[i] = SetWinEventHook(
            kRanges[i].lo, kRanges[i].hi, nullptr, &DockManager::WinEventProc,
            0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (m_winEventHooks[i]) ++installed;
    }
    if (installed == 0) {
        // 一条都没挂上（极端环境）：退回「只有兜底定时器」的降级模式仍可用，
        // 但恢复延迟会退化到 ~1.2s，这里显式记一条以便真机排查。
    }
    RecomputeOcclusion();   // 启动即算一次（开机时前台可能已是最大化窗口）
}

void DockManager::UninstallOcclusionHook() {
    for (int i = 0; i < OCCLUSION_HOOK_COUNT; ++i) {
        if (m_winEventHooks[i]) {
            UnhookWinEvent(m_winEventHooks[i]);
            m_winEventHooks[i] = nullptr;
        }
    }
    if (m_trayHwnd) {
        if (m_occlusionDebounceTimer) KillTimer(m_trayHwnd, TID_OCCLUSION_DEBOUNCE);
        if (m_occlusionFallbackTimer) KillTimer(m_trayHwnd, TID_OCCLUSION_FALLBACK);
        if (m_minimizeWatchdogTimer)  KillTimer(m_trayHwnd, TID_MINIMIZE_WATCHDOG);
    }
    m_occlusionDebounceTimer = 0;
    m_occlusionFallbackTimer = 0;
    m_minimizeWatchdogTimer = 0;
    // 只清自己那份所有权：无头用例会在同进程内构造多个 DockManager，
    // 不加这个判断会把别人的实例指针抹掉。
    if (g_occlusionOwner == this) g_occlusionOwner = nullptr;
}

// ───────────────────────── 托盘宿主窗口 ─────────────────────────
static constexpr const wchar_t* DM_TRAY_CLASS = L"openDockTrayHost";

void DockManager::CreateTrayHost() {
    if (m_trayHwnd) return;
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = StaticTrayWndProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = DM_TRAY_CLASS;
    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    }
    m_trayHostClass = true;
    m_trayHwnd = CreateWindowExW(0, DM_TRAY_CLASS, L"openDockTray",
                                 WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hinst, this);
}

void DockManager::AddTrayIcon() {
    if (!m_trayHwnd) return;
    ::ZeroMemory(&m_nid, sizeof(m_nid));
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd   = m_trayHwnd;
    m_nid.uID    = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_APP_TRAY;
    // 托盘图标：直接取 exe 内嵌资源 MAINICON（与文件/任务栏图标同一资源），
    // 保证托盘图标与应用程序图标 100% 一致，且不再依赖 exe 同目录的松散 tray_icon.png。
    // 资源名写法因 rc 工具链而异：先按字符串 "MAINICON"，失败再按整数 id 1 回退。
    // 加载失败回退系统默认图标。自定义句柄在 RemoveTrayIcon 中释放，避免 GDI 句柄泄漏。
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    HICON hTray = (HICON)LoadImageW(hinst, L"MAINICON", IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (!hTray) hTray = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    m_nid.hIcon = hTray ? hTray : LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    m_trayIconOwned = (hTray != nullptr);
    wcscpy_s(m_nid.szTip, L"openDock");
    if (Shell_NotifyIconW(NIM_ADD, &m_nid)) m_trayAdded = true;
}
void DockManager::RemoveTrayIcon() {
    if (m_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_trayAdded = false;
        if (m_trayIconOwned && m_nid.hIcon) {
            DestroyIcon(m_nid.hIcon);
            m_nid.hIcon = nullptr;
            m_trayIconOwned = false;
        }
    }
}

LRESULT CALLBACK DockManager::StaticTrayWndProc(HWND hwnd, UINT msg,
                                               WPARAM wParam, LPARAM lParam) {
    DockManager* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<DockManager*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DockManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->TrayWndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT DockManager::TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_APP_TRAY) {
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            POINT pt = {}; GetCursorPos(&pt);
            ShowTrayMenu(pt.x, pt.y);
        }
        return 0;
    }
    // 显示桌面(WorkerW/Progman 置前)检测：把 dock 抬到桌面之上，避免被盖住而失效。
    // 由 WinEventProc / 1s 看门狗在状态翻转时投递，此处落到消息循环安全执行 Z 序变更。
    if (msg == WM_APP_SHOWDESKTOP) {
        ApplyShowDesktopState(wParam != 0);
        return 0;
    }
    // 需求 6：去抖落盘定时器到点
    if (msg == WM_TIMER && wParam == TID_CFG_DEBOUNCE) {
        KillTimer(hwnd, TID_CFG_DEBOUNCE);
        FlushConfig();
        return 0;
    }
    // P0 遮挡检测：去抖定时器到点 —— 一串窗口事件（拖动一个窗口会连发几十条
    // LOCATIONCHANGE）合并成一次真正的几何计算。先 KillTimer 再算，避免
    // RecomputeOcclusion 内部若再次 ScheduleOcclusionRecompute 造成自激。
    if (msg == WM_TIMER && wParam == TID_OCCLUSION_DEBOUNCE) {
        KillTimer(hwnd, TID_OCCLUSION_DEBOUNCE);
        m_occlusionDebounceTimer = 0;
        RecomputeOcclusion();
        return 0;
    }
    // P0 遮挡检测：兜底定时器到点 —— 只在「至少一条边处于遮挡态」时存在。
    // 覆盖 WinEvent 收不到的解除路径（如上层窗口被 GPU 独占全屏程序顶掉、
    // 远程桌面重连、某些 UWP 只改 cloak 不发事件）。RecomputeOcclusion 内部
    // 会在全部解除后自行 KillTimer，故这里【不】主动关，只做重算。
    if (msg == WM_TIMER && wParam == TID_OCCLUSION_FALLBACK) {
        RecomputeOcclusion();
        return 0;
    }
    // 显示桌面 / Win+D / Win+M 兜底看门狗（多层防御第三层）：轮询四边是否被 OS 最小化，
    // 是则恢复。仅业务态仍要求可见（IsWindowVisibleForTest）才恢复，autoHide 隐藏态不误唤出。
    if (msg == WM_TIMER && wParam == TID_MINIMIZE_WATCHDOG) {
        for (int i = 0; i < 4; ++i) {
            DockEngine* e = m_docks[i].get();
            if (!e) continue;
            HWND h = e->GetHwnd();
            if (h && IsIconic(h) && e->IsWindowVisibleForTest())
                e->RestoreFromOsMinimize();
        }
        // 第三层兜底：显示桌面(WorkerW/Progman 置前)不仅可能最小化 dock，更常见的是用
        // 桌面窗口「盖住」dock（dock 从不进入最小化态、IsIconic 恒 false）。兼做前台桌面
        // 检测，漏网时 1s 内也能把 dock 抬到桌面之上；仅状态翻转才真正改 Z 序。
        OnForegroundWindowChanged(GetForegroundWindow());
        return 0;
    }
    // P2-9 多显示器：显示器增删 / 分辨率 / 缩放变化后，各边 footprint 所在的
    // HMONITOR 与 work area 都可能整体改变（IsFootprintOccluded 里的
    // GetFootprintWorkArea、ComputeRevealZoneFor 里的 GetMonitorWorkRect 都依赖它），
    // 而这类变化不一定伴随任何 WinEvent，遮挡态可能长期停留在旧判定上。
    // 这里只做一次去抖重算：不臆造 dock 跨屏迁移路径（引擎侧几何由各自 Apply* 负责），
    // 走 ScheduleOcclusionRecompute 而非直接 RecomputeOcclusion，是因为拓扑切换瞬间
    // 系统会连发多条 WM_DISPLAYCHANGE + 大量窗口 LOCATIONCHANGE，此刻 z 序与窗口矩形
    // 都还在抖动，等 120ms 稳定后再算一次结果才可信。
    if (msg == WM_DISPLAYCHANGE) {
        ScheduleOcclusionRecompute();
        return 0;
    }
    // 需求 6：注销 / 关机保命。WM_QUERYENDSESSION 先落盘再放行；WM_ENDSESSION 再兜一次。
    // 旧实现这两条消息完全没处理，关机时最后一次改动必丢。
    if (msg == WM_QUERYENDSESSION) {
        FlushConfig(/*force=*/true);
        return TRUE;
    }
    if (msg == WM_ENDSESSION) {
        if (wParam) FlushConfig(/*force=*/true);
        return 0;
    }
    if (msg == WM_COMMAND) {
        int id = (int)LOWORD(wParam);
        if (id == ID_TRAY_EXIT) {
            FlushConfig(/*force=*/true);   // 退出前强制落盘（补 D9c）
            RemoveTrayIcon();
            PostQuitMessage(0);
        } else if (id == ID_TRAY_AUTOSTART) {
            // 需求 7：唯一的 UI 入口。此前全工程没有任何地方能改 autoStart，
            // 用户永远打不开 —— 这才是"开机自启未能有效"最直接的原因。
            const bool want = !m_cfg.autoStart;
            const bool ok   = want ? AutoStart::Enable() : AutoStart::Disable();
            if (ok) {
                m_cfg.autoStart = want;
                FlushConfig(/*force=*/true);   // 立即落盘，别等去抖
            } else {
                MessageBoxW(m_trayHwnd, L"无法修改开机自启：注册表写入被策略或安全软件阻止。",
                            L"openDock", MB_OK | MB_ICONWARNING);
            }
        } else if (id == DockEngine::ID_ZORDER_FRONT)  { SetZOrder(1); }
        else if (id == DockEngine::ID_ZORDER_NORMAL) { SetZOrder(0); }
        else if (id == DockEngine::ID_ZORDER_BACK)   { SetZOrder(-1); }
        else if (id == ID_TRAY_EDGE_TOP)    { SetEdgeEnabled(DockPosition::Top,    !IsEdgeEnabled(DockPosition::Top)); }
        else if (id == ID_TRAY_EDGE_BOTTOM) { SetEdgeEnabled(DockPosition::Bottom, !IsEdgeEnabled(DockPosition::Bottom)); }
        else if (id == ID_TRAY_EDGE_LEFT)   { SetEdgeEnabled(DockPosition::Left,   !IsEdgeEnabled(DockPosition::Left)); }
        else if (id == ID_TRAY_EDGE_RIGHT)  { SetEdgeEnabled(DockPosition::Right,  !IsEdgeEnabled(DockPosition::Right)); }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void DockManager::ShowTrayMenu(int screenX, int screenY) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    HMENU hEdges = CreatePopupMenu();
    auto addEdge = [&](UINT id, DockPosition e, const wchar_t* name) {
        UINT flags = MF_STRING;
        if (IsEdgeEnabled(e)) flags |= MF_CHECKED;
        AppendMenuW(hEdges, flags, id, name);
    };
    addEdge(ID_TRAY_EDGE_BOTTOM, DockPosition::Bottom, L"底部");
    addEdge(ID_TRAY_EDGE_TOP,    DockPosition::Top,    L"顶部");
    addEdge(ID_TRAY_EDGE_LEFT,   DockPosition::Left,   L"左侧");
    addEdge(ID_TRAY_EDGE_RIGHT,  DockPosition::Right,  L"右侧");
    AppendMenuW(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hEdges, L"显示边");

    // 图层位置（总在前面 / 正常 / 总在后面）；默认（配置）为「总在后面」
    {
        HMENU hZ = CreatePopupMenu();
        AppendMenuW(hZ, MF_STRING | (m_cfg.zOrder == 1  ? MF_CHECKED : 0), DockEngine::ID_ZORDER_FRONT,  L"总在前面");
        AppendMenuW(hZ, MF_STRING | (m_cfg.zOrder == 0  ? MF_CHECKED : 0), DockEngine::ID_ZORDER_NORMAL, L"正常");
        AppendMenuW(hZ, MF_STRING | (m_cfg.zOrder == -1 ? MF_CHECKED : 0), DockEngine::ID_ZORDER_BACK,   L"总在后面");
        AppendMenuW(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hZ, L"图层位置");
    }

    // 需求 7：开机自动启动。勾选状态以【注册表实际状态】为准（用户看到的是"现在到底
    // 会不会自启"），而点击写入的意图落到 config —— intent/state 分离，见 ADR §3.3。
    {
        const AutoStart::Query q = AutoStart::Read();
        UINT flags = MF_STRING;
        if (q.status == AutoStart::Status::EnabledCurrent) flags |= MF_CHECKED;
        if (q.status == AutoStart::Status::Error)          flags |= MF_GRAYED;
        AppendMenuW(hMenu, flags, ID_TRAY_AUTOSTART, L"开机自动启动");
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");

    SetForegroundWindow(m_trayHwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, screenX, screenY, 0, m_trayHwnd, nullptr);
    DestroyMenu(hMenu);
}

// 需求：托盘菜单移除「添加应用到所有边 / 添加文件夹到所有边」，对应对话框入口一并移除。
