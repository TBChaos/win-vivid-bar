// src/app/DockManager.h
// 多 Dock 编排器（#4 升级：四边同时显示）
// 为每条【启用】的边创建一个独立的 DockEngine 实例（每边一个 Dock 栏，各自图标集 /
// 悬停放大 / 自动隐藏 / 拖放 / 右键菜单），由本类统一运行单一消息循环、持有单个
// 系统托盘入口，并支持运行时按边开关（#3 多实例版）。
#pragma once
#include "../Common.h"
#include "ConfigManager.h"
#include <shellapi.h>   // NOTIFYICONDATA / Shell_NotifyIcon（系统托盘）
#include <array>
#include <memory>
#include <string>
#include <mutex>        // std::mutex（边所有权锁，P1-7 换边竞态串行化）

class DockEngine;

class DockManager {
public:
    DockManager();
    ~DockManager();

    HRESULT Initialize(const AppConfig& cfg);
    HRESULT InitializeFromFile(const std::string& configPath);
    HRESULT ShowAll();
    HRESULT HideAll();
    int  Run();          // 单一消息循环（Windowed 交互模式）
    void Shutdown();

    // 运行时按边开关（#3 多实例版）：启用即创建该边 Dock 并弹出，禁用即销毁
    void SetEdgeEnabled(DockPosition edge, bool enabled);
    void SetZOrder(int z);     // 图层位置（1=前 0=正常 -1=后），即时生效并落盘

    // --force-gdi：强制各边引擎走 GDI 三级降级回退。
    // 必须在 Initialize / InitializeFromFile 之前调用 —— 引擎在 CreateEdgeEngine 内建立，
    // DockEngine::Initialize 会把该标志下发给 RenderManager，之后再设无效。
    void SetForceGdiFallback(bool b) { m_forceGdi = b; }
    void MoveHomeEdge(DockPosition from, DockPosition to);  // P1-7：原子交接——旧引擎先释放再创建新引擎，杜绝同边双引擎

    size_t EngineCount() const;                       // 当前活跃 Dock 数
    DockEngine* GetEngine(DockPosition edge) const;   // 取某边引擎（无则 nullptr）

private:
    static LRESULT CALLBACK StaticTrayWndProc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam);
    LRESULT TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void CreateTrayHost();                 // 隐藏宿主窗口（接收托盘回调）
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(int screenX, int screenY);

    bool  IsEdgeEnabled(DockPosition edge) const;
    bool& EdgeRef(DockPosition edge);

    // ═══ P0 遮挡检测（事件驱动惰性重算，见 plan-occlusion-idle-cpu-2026-08-06.md）═══
    // 反对每帧 EnumWindows（O(窗口数) 反成新的常驻成本）：仅在系统窗口事件到达时
    // 去抖重算，遮挡期额外挂一个低频兜底定时器防「漏事件卡死」。
    void InstallOcclusionHook();        // 在消息泵线程注册 SetWinEventHook（Run 内 CreateTrayHost 之后）
    void UninstallOcclusionHook();      // UnhookWinEvent + 关两个 SetTimer（Shutdown 内）
    void ScheduleOcclusionRecompute();  // 事件回调 → 一次性去抖定时器（合并抖动）
    void RecomputeOcclusion();          // 遍历各存活边，逐边 SetOccluded + 维护兜底定时器
    // 单条边的 footprint 是否被完全遮挡。self = 该边窗口（用于沿 z 序只看【上层】窗口）。
    bool IsFootprintOccluded(HWND self, const RECT& foot) const;
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG idObject, LONG idChild,
                                      DWORD idEventThread, DWORD dwmsEventTime);

    // 显示桌面(WorkerW/Progman 置前)检测与恢复：被桌面窗口「盖住」的 dock 抬到桌面之上，
    // 避免其从不进入最小化态(IsIconic 恒 false)而无法靠遮挡/最小化逻辑恢复。
    void OnForegroundWindowChanged(HWND fg);
    void ApplyShowDesktopState(bool desktop);
    // 应用【有效 Z 序】：显示桌面激活时把 dock 抬到桌面窗口(WorkerW/Progman)之上，
    // 但保留其配置的相对图层（topmost 配置维持 HWND_TOPMOST；bottom/normal 仅置于桌面窗口之上，
    // 仍位于所有普通应用之下，等效「总在后面」）；非激活时按配置 m_zOrder 正常落位。
    void ApplyEffectiveZOrder(DockEngine* e, bool desktop);

    HRESULT CreateEdgeEngine(DockPosition edge);   // 创建并初始化某边引擎
    void    DestroyEdgeEngine(DockPosition edge);  // 销毁某边引擎

    AppConfig m_cfg;                                  // 主配置（含 edgeIcons / 全局设置）
    std::array<std::unique_ptr<DockEngine>, 4> m_docks;  // 按 DockPosition 索引（0..3）
    std::unique_ptr<ConfigManager> m_cfgMgr;
    std::string m_configPath;                         // 解析后的持久化目标
    bool m_initialized = false;
    bool m_forceGdi = false;                          // --force-gdi：强制 GDI 回退

    // P1-7：边所有权锁，串行化边开关 / 换边，确保任意时刻同边仅一个活跃引擎。
    std::mutex m_edgeMutex;

    // #3 独立存储：权威共享图标存储（每个引擎通过 shared_ptr 直接读写自己的槽位，
    // 避免单引擎 SaveConfig 写全量 config 时把它边陈旧副本覆盖）。每条边独立编排、互不干扰。
    std::shared_ptr<std::array<std::vector<IconEntry>, 4>> m_sharedEdgeIcons;
    std::shared_ptr<std::vector<IconEntry>> m_sharedSharedIcons;   // 共享默认图标集（共享）

    // #3 统一落盘入口：合并各引擎通过回调传来的配置（以共享图标存储为准）写完整四边配置
    void SaveConfigTo(const AppConfig& fromEngine);

public:
    // ═══ 配置持久化（需求 6）═══
    // 置脏 + 重置去抖定时器（拖拽重排会连续触发几十次，去抖后一次拖拽只写一次盘）
    void MarkConfigDirty();
    // 立即落盘。force=true 时忽略脏标记（退出 / 关机注销的兜底路径必须用它）。
    // 返回是否写盘成功；返回 false 表示用户改动【没有】保存下来。
    bool FlushConfig(bool force = false);

private:
    // 从各边【活跃引擎的当前 icons】刷新 m_cfg / 共享存储。
    // 不能信 m_cfg.sharedIcons：多实例路径下 IconSetManager::SyncCurrentEdgeIcons 因
    // m_sharedSharedIcons 非空而跳过 sharedIcons 更新，它永远停在加载初值。
    void RefreshIconsFromEngines();

    bool m_cfgDirty = false;

    // 托盘宿主（隐藏窗口，仅用于接收 Shell_NotifyIcon 回调与菜单命令）
    HWND m_trayHwnd = nullptr;
    NOTIFYICONDATAW m_nid = {};
    bool m_trayAdded = false;
    bool m_trayIconOwned = false;   // 自定义托盘图标句柄（LoadTrayIcon）需 DestroyIcon
    bool m_trayHostClass = false;

    static constexpr UINT WM_APP_TRAY       = WM_APP + 2;
    static constexpr UINT WM_APP_SHOWDESKTOP = WM_APP + 11;  // 显示桌面(WorkerW/Progman 置前)检测
    static constexpr UINT ID_TRAY_EXIT      = 1001;
    static constexpr UINT ID_TRAY_EDGE_TOP    = 1201;
    static constexpr UINT ID_TRAY_EDGE_BOTTOM = 1202;
    static constexpr UINT ID_TRAY_EDGE_LEFT   = 1203;
    static constexpr UINT ID_TRAY_EDGE_RIGHT  = 1204;
    static constexpr UINT ID_TRAY_AUTOSTART   = 1301;   // 需求 7：开机自动启动（勾选项）

    // 配置去抖落盘定时器（托盘宿主窗口上）
    static constexpr UINT_PTR TID_CFG_DEBOUNCE = 1;
    static constexpr UINT     CFG_DEBOUNCE_MS  = 800;

    // ═══ P0 遮挡检测：钩子 / 定时器状态（全部只在消息泵线程读写，无需加锁）═══
    // 窗口事件钩子。刻意拆成【多个窄区间】而非一个 0x0003~0x8018 的大区间：
    // 后者会把 NAMECHANGE / VALUECHANGE / STATECHANGE 等高频无关事件也订阅进来，
    // 与「遮挡态 CPU 归零」的目标背道而驰。区间见 InstallOcclusionHook。
    static constexpr int OCCLUSION_HOOK_COUNT = 5;
    HWINEVENTHOOK m_winEventHooks[OCCLUSION_HOOK_COUNT] = {};
    UINT_PTR m_occlusionDebounceTimer = 0;   // 事件去抖（一次性，到点即毁）
    UINT_PTR m_occlusionFallbackTimer = 0;   // 兜底重检（仅遮挡期运行，全解除即 Kill）
    UINT_PTR m_minimizeWatchdogTimer = 0;    // 显示桌面等多层防御第三层：低频 IsIconic 兜底恢复
    static constexpr UINT_PTR TID_OCCLUSION_DEBOUNCE = 2;
    static constexpr UINT_PTR TID_OCCLUSION_FALLBACK = 3;
    // 去抖 120ms：低于人眼对「拖窗停手→dock 响应」的感知阈值，又足以把
    // LOCATIONCHANGE 连发（拖动窗口时每帧一条）合并成一次重算。
    static constexpr UINT OCCLUSION_DEBOUNCE_MS = 120;
    // 兜底 1200ms：只做布尔重检、不 PostMessage 驱动动画，且【仅遮挡期】运行。
    // 代价是最坏情况遮挡解除后 ~1.2s 才恢复看门狗（正常路径由事件钩子秒回）。
    static constexpr UINT OCCLUSION_FALLBACK_MS = 1200;

    // ═══ 显示桌面 / Win+D / Win+M 兜底看门狗（第三层防御，消息拦截漏网时的最后防线）═══
    // 1s 低频轮询四边 IsIconic：dock 是 WS_EX_TOOLWINDOW 无任务栏按钮，被 OS 最小化后
    // 无法手工恢复，故需自恢复。仅当业务态仍要求可见（IsWindowVisibleForTest）才恢复，
    // autoHide 隐藏态不误唤出。代价是空闲期线程每 1s 被唤醒一次（相较遮挡优化前的 40 次/秒
    // 可忽略），换来「任意最小化路径都可在 ~1s 内复活」。
    static constexpr UINT_PTR TID_MINIMIZE_WATCHDOG = 4;
    static constexpr UINT     MINIMIZE_WATCHDOG_MS  = 1000;

    // 显示桌面状态：前台窗口为桌面背板(WorkerW/Progman)时为 true。
    // dock 是 WS_EX_TOOLWINDOW 无任务栏按钮，显示桌面会把桌面窗口置前并盖住 dock；
    // dock 从不进入最小化态，故靠前台类检测，检测到即抬升 dock 到桌面之上。
    bool m_showDesktopActive = false;
    HWND m_desktopHwnd = nullptr;   // 当前置前的桌面窗口(WorkerW/Progman)，用于精准插入其上方
};
