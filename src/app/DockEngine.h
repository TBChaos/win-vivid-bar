// src/app/DockEngine.h
// 主控制器 — 生命周期 / 状态机 / 动画驱动 / 子系统协调
// 设计参考：详细设计说明 §2.1、§三（动画状态机）
#pragma once
#include "../Common.h"
#include <shellapi.h>   // NOTIFYICONDATA / Shell_NotifyIcon（系统托盘）
#include "../core/SpringSystem.h"
#include "../core/LayoutEngine.h"
#include "../core/HitTestEngine.h"
#include "../render/RenderManager.h"
#include "../platform/WindowManager.h"
#include "ConfigManager.h"
#include "IconProvider.h"
#include "../core/EdgeGeometry.h"   // 统一四边几何（IEdgeGeometry / MakeGeometry）
#include "../core/EdgeConfig.h"     // 四边独立配置（EdgeConfig / DockConfigStore）
#include <functional>   // std::function（多实例持久化回调）
#include <mutex>        // std::mutex（换边/边开关串行化，P1-7）

// ═══ T10 拆分：子模块前向声明 ═══
// 方法实现已下沉到这三个子模块类（见各自 .h/.cpp），DockEngine 经 unique_ptr 持有并薄转发。
class DockStateMachine;
class IconSetManager;
class DockInteraction;

// 每图标的稳定弹簧绑定（P0-3）：用「图标 key(path)」而非「图标下标」映射弹簧节点，
// 使增删/重排仅对 Δ 图标增删弹簧，稳定图标保留 value/velocity（无整体 SpringSystem 销毁重建）。
struct IconSpringBinding {
    uint32_t    scaleId   = 0;
    uint32_t    offsetId  = 0;
    uint32_t    opacityId = 0;
    std::wstring key;       // 图标 path（身份匹配）
};

class DockEngine {
public:
    DockEngine();
    ~DockEngine();

    // ═══ 生命周期 ═══
    HRESULT Initialize(const AppConfig& config);
    HRESULT InitializeFromFile(const std::string& configPath);  // 缺省回退默认配置
    HRESULT Show();
    HRESULT Hide();
    void Shutdown();

    // ═══ 消息处理（Windowed）═══
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    int Run();                                 // 消息循环（Release 形态）

    // ═══ 动画驱动 ═══
    void OnAnimationTick(float deltaTime);

    // ═══ 查询 ═══
    DockState GetState() const { return m_state; }
    HWND GetHwnd() const { return m_window ? m_window->GetHwnd() : nullptr; }
    int  GetHoveredIndex() const { return m_hoveredIndex; }
    const AppConfig& GetConfig() const { return m_appConfig; }
    bool AreSpringsFinite() const;   // 调试/验收：所有弹簧数值有限（无 NaN/Inf）
    bool AreSpringsSettled() const;   // 调试/验收：所有弹簧已收敛
    bool IsAnyScaleElevated() const { return AnyScaleElevated(); }  // #1 验收：是否有图标卡在放大态
    // Bugfix 回归辅助：所有图标 scale 目标的最小值。1.0=正常大小；
    // 趋近 0 = 被 ApplyExitTargets 缩到最小（即用户报障的「过度缩小」）。
    float GetMinIconScaleTarget() const;
    const char* GetStateName() const; // 调试/验收：当前状态机状态名

    // ═══ 性能验收（Step 11：无头性能基线）═══
    // 累计弹簧积分 / 布局计算耗时（微秒）与计帧数；Reset 后可在 1000 帧模拟前后读取差值
    void ResetPerfAccum();
    void GetPerfAccum(double& springUs, double& layoutUs, long long& frames) const;
    double GetPerfRenderUs() const;   // 委托 RenderManager 的提交耗时累计

    // ═══ 启动目标（Step 1：点击启动应用）═══
    std::wstring GetResolvedLaunchTarget(int index) const;  // 解析后的启动路径
    bool        IsLaunchTargetValid(int index) const;       // 配置提供了有效目标
    bool        LaunchIcon(int index);                       // 通过 ShellExecute 启动

    // ═══ 窗口效果（Step 5：Acrylic 毛玻璃 + 圆角）═══
    int  GetBlurMode() const;        // 0=off 1=acrylic 2=accent blur 3=dwm blur
    bool IsWindowRounded() const;

    // ═══ DPI / 多显示器（Step 6）═══
    unsigned int GetWindowDpi() const;   // 窗口 DPI（无窗口时 96）
    int          GetMonitorCount() const;

    // ═══ 自动隐藏 + 空闲鼠标穿透（Step 7）═══
    bool IsAutoHideEnabled() const { return m_autoHide; }
    bool IsMousePenetrating() const { return m_mousePenetrating; }
    bool IsHidden() const { return m_state == DockState::Hidden; }
    int  GetShowDelayMs() const { return m_showDelayMs; }
    int  GetHideDelayMs() const { return m_hideDelayMs; }
    void SetAutoHideEnabled(bool on);   // 运行时切换（托盘菜单等）

    // ═══ 遮挡挂起（P0：被其它窗口完全遮挡时 CPU 归零）═══
    // 背景：被遮挡时唯一常驻 CPU 源是 100ms 看门狗轮询（四边 ~40 次/秒）。
    // m_occluded 由编排层 DockManager::RecomputeOcclusion 事件驱动写入；
    // UpdateIdleWatchdog 判据【整条】前置 !m_occluded —— 必须覆盖
    // autoHide / 穿透 / 放大三个分支，因为非 autoHide 常显模式下
    // HandleMouseLeave → SetPenetration(true) 同样会让看门狗常驻。
    void SetOccluded(bool on);          // 置遮挡态（挂起 / 恢复看门狗）
    bool IsOccluded() const { return m_occluded; }
    // P1-6 无头验证钩子：本次遮挡挂起是否由【我们】主动 Show(false) 隐藏了窗口。
    // 用于区分「autoHide 自身的隐藏态」（此值恒 false，解除遮挡时绝不 Show(true)）
    // 与「非 autoHide 常显态被我们收起来释放 DComp 合成」（此值为 true，需还原）。
    bool DidOcclusionHideWindow() const { return m_occlusionHidWindow; }
    // 底层窗口当前是否可见（由 WindowManager::Show 维护）
    bool IsWindowVisibleForTest() const { return m_window && m_window->IsVisible(); }
    // 「按当前判据看门狗是否应运行」，与 StartWatchdog/StopWatchdog 分支条件逐字一致
    bool IsWatchdogActive() const {
        return !m_occluded && (m_autoHide || m_mousePenetrating || AnyScaleElevated());
    }

    // ═══ 右键删除 + 拖拽排序 + 拖拽添加（Step 8）═══
    int  GetIconCount() const { return (int)m_appConfig.icons.size(); }
    DockPosition GetDockPosition() const { return m_appConfig.dock.position; }
    int  GetIconTextureCount() const;   // 已成功加载的图标纹理数（调试/验收用）
    bool IsDragging() const { return m_dragging; }  // 拖拽状态查询（无头验证用）
    std::wstring GetIconPath(int index) const;
    std::wstring GetIconName(int index) const;
    bool RemoveIcon(int index, bool persist = true);    // 右键删除
    bool ReorderIcon(int from, int to, bool persist = true); // 拖拽排序
    void LiveDragReorder(const POINT& pt);                  // 拖拽实时重排预览（不落盘）
    bool AddIcon(const std::wstring& path, const std::wstring& name = L"",
                 bool persist = true, int insertAt = -1); // 拖拽添加；insertAt>=0 插入到指定下标
    void PersistConfigTo(const std::string& path) const; // 写入当前配置（验证用）
    // 文件拖放回调（IDropTarget）
    void AddIconFromDrop(const std::wstring& path, int insertAt = -1);

    // 外部拖放实时预览（IDropTarget 进行中）：拖入时光标槽位插入半透明占位、其余图标让位、
    // 被拖（占位）图标跟随光标；落盘前占位转正 / 离开撤销。与「拖拽改变顺序」共用插入位算法。
    void BeginExternalDropPreview(const std::vector<std::wstring>& paths, POINT pt);
    void MoveExternalDropPreview(POINT pt);
    void EndExternalDropPreview(bool commit);   // commit=true 占位转正并落盘；false 撤销
    bool ExternalDragPreviewActive() const { return m_externalDragActive; }
    // 调试日志（真机 GUI 缺陷取证用，落盘到 <exedir>/debug_output/openDock.log）
    void DebugLog(const wchar_t* fmt, ...);

    // ═══ 开机自启动 + 位置微调 + 图层 Z 序（Step 10）═══
    void ApplyPlacement();               // 依配置(position/offset/monitor/zOrder)定位窗口
    int  GetWindowZOrder() const;        // 当前 Z 序（1=topmost 0=normal -1=bottom）
    RECT GetDockScreenRect() const;      // 基础 Dock 条屏幕矩形（验证用）
    // 运行时修改偏移/Z 序并立即应用（设置面板/验证用）
    void SetPlacementOverride(int edgeOffset, int centerOffset, int zOrder);
    // 运行时修改停靠边（top/left/right/bottom）并立即应用（右键菜单/验证用）
    void SetDockPosition(DockPosition pos);
    // #3：四边吸附/感应区独立开关（启用即把 dock 吸附到该边；不能全禁用）
    void SetEdgeEnabled(DockPosition edge, bool enabled);
    bool IsEdgeEnabled(DockPosition edge) const;   // #3 该边是否启用（只读查询，供验证/状态读取）
    // 运行时设置（右键菜单/验证用），改动实时生效并持久化（m_configPath 非空时）
    void SetIconSize(float size);                 // 大小：小/中/大
    void SetBackgroundOpacity(float opacity);      // 透明度（背景条）
    bool AddAppViaDialog();                        // 文件对话框添加应用
    bool AddFolderViaDialog();                     // #2 文件夹选择对话框添加文件夹
    void SetZOrder(int zOrder);                    // #5 图层位置：1=前 0=正常 -1=后
    void SetDockBarVisible(bool visible);          // #N 切换 Dock 底座背景条显隐
    void SetConfigPath(const std::string& path);
    // 多实例（DockManager）：由编排器提供的持久化回调与共享图标存储，确保每条边
    // 独立保存、互不覆盖（#3 独立存储）。单实例（无头/旧路径）下可留空，走本地 SaveConfig。
    void SetPersistCallback(std::function<void(const AppConfig&)> cb);
    void SetSharedIcons(std::shared_ptr<std::array<std::vector<IconEntry>, 4>> edges,
                        std::shared_ptr<std::vector<IconEntry>> shared);
    void PersistConfig() const;                    // 统一持久化入口（优先回调）
    void SetForceGdiFallback(bool b) { m_forceGdi = b; }   // 测试钩子：--force-gdi 强制 GDI 回退   // 持久化目标（空=不写盘）
    const std::string& GetConfigPath() const { return m_configPath; }
    float GetDockWidth()  const { return m_dockWidth; }
    float GetDockHeight() const { return m_dockHeight; }
    // P0-4：角格边长 = min(dockWidth, dockHeight)（相邻两带法向厚度交叠正方形，取较小带厚）
    int  ComputeCornerSize() const;
    // Bugfix 回归辅助：只读暴露【本边】感应区矩形（内部 ComputeRevealZoneFor 为 private），
    // 便于断言「感应区确实落在对应边」并打印真实坐标做证据。
    RECT GetOwnRevealZoneForTest() const;

    // Step 14 验证辅助：取某图标静息态的屏幕中心（朝向感知，用于无头命中测试）
    bool GetIconScreenCenter(int index, float& screenX, float& screenY) const;
    // Step 14 验证辅助：取某图标当前（动画后）屏幕中心
    bool GetIconCurrentScreenCenter(int index, float& screenX, float& screenY) const;
    // Step 14 验证辅助：读取当前布局某图标的规范坐标（x=主轴, y=内为正）
    bool GetLayout(int index, float& mainX, float& crossY) const;
    // INV-ENVELOPE 验证辅助：读取当前（动画后）布局某图标的 scale。
    // 包络不变量测试需要「引擎实际达成的 scale」而非测试侧重算的鱼眼公式 ——
    // 后者会让测试与被测代码互相印证而永远不红（正是 D1/C2 漏网的原因）。
    bool GetIconCurrentScale(int index, float& scale) const {
        if (index < 0 || index >= (int)m_currentLayouts.size()) return false;
        scale = m_currentLayouts[(size_t)index].scale;
        return true;
    }
    int GetCurrentLayoutCount() const { return (int)m_currentLayouts.size(); }
    // Bug #2 验证/逻辑入口：以屏幕坐标判定该点是否应命中 Dock（返回 true=HTCLIENT）。
    // 与 WM_NCHITTEST 共用同一套「Dock 条矩形 + 图标真实屏幕矩形」命中逻辑，便于无头探针断言。
    bool HitTestAt(int x, int y);

    // ═══ Step 12：放大溢出留白（依停靠边 + maxScale 计算四边留白）═══
    void ComputeInsets(int& left, int& top, int& right, int& bottom) const;
    void GetContentInsets(int& left, int& top, int& right, int& bottom) const {
        left = m_insetL; top = m_insetT; right = m_insetR; bottom = m_insetB;
    }

private:
    // 状态机
    void EnterState(DockState next);
    void UpdateStateMachine(bool allSettled);

    // 交互处理
    void HandleMouseMove(int screenX, int screenY);
    void HandleMouseLeave();
    void HandleClick(int screenX, int screenY);
    void TriggerBounce(int iconIndex);

    // 弹簧目标设置
    void ApplyHoverTargets(float mouseXCentered);   // 鱼眼放大
    void ApplyRestTargets();                        // 全部回归静息
    bool AnyScaleElevated() const;                  // #1 看门狗：是否有图标处于放大态
    bool IsFisheyeEnabled() const;                  // #N 当前边是否启用鱼眼放大
    void ApplyEntryTargets(int iconIndex);          // 入场（单图标，级联用）
    void ApplyExitTargets();

    // P0-1/2/3：静息布局缓存 + 稳定弹簧绑定增量重建
    const std::vector<IconLayout>& EnsureRestLayout() const;   // 脏标记缓存（零弹簧热路径）
    void ReconcileSprings(const std::vector<IconEntry>& newIcons, float initScaleOpacity); // 增量重建

    // Step 14：命中测试坐标适配已在 HitTestEngine 内通过 MapDockLayout 完成
    //          （支持水平/竖直朝向），不再需要 ToDockLocal。

    void StartAnimationLoop();
    void StopAnimationLoop();

    // ═══ 自动隐藏 + 空闲鼠标穿透（Step 7）═══
    // 设置/取消鼠标穿透；并据 autoHide/穿透状态维持低频率看门狗
    void SetPenetration(bool penetrate);
    void UpdateIdleWatchdog();          // 依据 (autoHide || 穿透) 启停看门狗
    void StartWatchdog();
    void StopWatchdog();
    void TickIdle(float dt);            // 看门狗回调：推进显示/隐藏倒计时 + 探测光标
    void AdvanceAutoHide(float dt);     // 推进 show/hide 倒计时（动画帧与看门狗共用）
    // 计算"边缘感应区"矩形（Dock 矩形向屏幕内侧扩展 dockHeight）
    RECT ComputeRevealZone() const;
    RECT ComputeRevealZoneFor(DockPosition edge, RECT dockRect) const;  // #3 指定边
    DockPosition NextEnabledEdge(DockPosition avoid) const;             // #3 下一个启用边
    bool& EdgeRef(DockPosition edge);                                   // #3 边开关引用
    void ApplyDockPosition(DockPosition pos);                         // P1-7：受锁保护的换边实现（SetDockPosition/SetEdgeEnabled 内部调用，避免重入死锁）
    // 计算拖拽释放时的插入位（原始图标索引序号）
    int  ComputeDragInsertIndex(POINT pt);

    // 系统托盘图标（常驻 dock 的退出入口，仅 Windowed 模式）
    void AddTrayIcon();
    void RemoveTrayIcon();

    // Step 13：右键菜单命令分发
    void HandleMenuCommand(int cmd);

    // ═══ 子系统 ═══
    std::unique_ptr<SpringSystem>  m_springs;
    std::unique_ptr<LayoutEngine>  m_layout;
    std::unique_ptr<HitTestEngine> m_hitTest;
    std::unique_ptr<RenderManager> m_render;
    std::unique_ptr<WindowManager> m_window;
    std::unique_ptr<ConfigManager> m_configMgr;
    std::unique_ptr<IconProvider>  m_iconProvider;

    // ═══ T10 拆分：子模块（薄转发 + friend）═══
    // 所有拆分方法实现已下沉到子模块类，DockEngine 仅持有并薄转发；
    // 子模块经 m_owner（DockEngine*）反查本类私有成员，故声明为 friend。
    friend class DockStateMachine;
    friend class IconSetManager;
    friend class DockInteraction;
    friend class DockManager;
    friend class DockDropTarget;   // 文件拖放：Drop 内需 ComputeDragInsertIndex 计算插入位
    std::unique_ptr<DockStateMachine> m_stateMachine;
    std::unique_ptr<IconSetManager>   m_iconSet;
    std::unique_ptr<DockInteraction>  m_interaction;

    // ═══ 配置 ═══
    AppConfig  m_appConfig;
    float m_dockWidth  = 0.0f;
    float m_dockHeight = 0.0f;
    // Step 12：当前生效的四边留白（px），用于验证与调试导出
    int m_insetL = 0, m_insetT = 0, m_insetR = 0, m_insetB = 0;

    // ═══ 统一配置 C++ 模块（EdgeGeometry / EdgeConfig）═══
    std::unique_ptr<IEdgeGeometry> m_geom;        // 当前边几何（编译期模板 + 多态）
    std::array<EdgeConfig, 4>      m_edgeConfigs; // 四边独立配置（defaults + edges 继承）

    // Step 13：Dock 右键菜单命令（与托盘菜单 ID_TRAY_EXIT=1001 不冲突）
    static constexpr UINT ID_DOCK_ADD     = 2001;
    static constexpr UINT ID_DOCK_REMOVE  = 2002;
    static constexpr UINT ID_DOCK_ADD_FOLDER = 2003;   // #2 添加文件夹
    static constexpr UINT ID_POS_TOP    = 2101, ID_POS_BOTTOM = 2102,
                                  ID_POS_LEFT   = 2103, ID_POS_RIGHT  = 2104;
    static constexpr UINT ID_ZORDER_FRONT = 2401, ID_ZORDER_NORMAL = 2402,
                                  ID_ZORDER_BACK   = 2403;   // #5 图层位置
    static constexpr UINT ID_SIZE_S     = 2201, ID_SIZE_M     = 2202, ID_SIZE_L    = 2203;
    static constexpr UINT ID_OPACITY_25 = 2301, ID_OPACITY_50 = 2302,
                                  ID_OPACITY_75 = 2303, ID_OPACITY_100 = 2304;
    static constexpr UINT ID_BAR_TOGGLE = 2501;   // #N 显示/隐藏 Dock 底座背景条
    static constexpr float SIZE_SMALL = 40.0f, SIZE_MED = 56.0f, SIZE_LARGE = 72.0f;

    // ═══ 状态 ═══
    DockState m_state = DockState::Hidden;
    int   m_hoveredIndex = -1;
    bool  m_mouseInDock  = false;
    POINT m_lastMousePos = {};
    float m_stateTime    = 0.0f;    // 当前状态累计时间（入场级联/统计）
    int   m_entryReleased = 0;      // 已释放入场动画的图标数
    float m_bounceResetTimer = -1.0f;  // >0 时倒计时，归零后弹跳目标回 0
    int   m_bounceIconIndex  = -1;
    std::vector<IconLayout> m_currentLayouts;
    std::vector<std::wstring> m_iconNames;   // 图标显示名缓存（Tooltip 用）

    // P0-1/2：静息布局脏标记缓存（零弹簧热路径查表）
    mutable std::vector<IconLayout> m_restLayouts;
    mutable bool m_restDirty = true;
    // P0-3：每图标稳定弹簧绑定（key=图标 path），与 m_appConfig.icons 顺序一致
    std::vector<IconSpringBinding> m_iconSprings;

    // ═══ 定时器（Windowed）═══
    HANDLE m_timerHandle = nullptr;
    bool   m_isAnimating = false;
    LARGE_INTEGER m_lastTickTime  = {};
    LARGE_INTEGER m_perfFrequency = {};

    // ═══ 自动隐藏 + 空闲鼠标穿透（Step 7）═══
    bool  m_autoHide      = false;     // 自动隐藏开关
    int   m_showDelayMs   = 0;         // 靠近边缘后弹出的延迟（ms）
    int   m_hideDelayMs   = 300;       // 鼠标离开后隐藏的延迟（ms）
    bool  m_mousePenetrating = false;  // 当前是否点击穿透（空闲时为 true）
    // P0 遮挡挂起：本边 footprint 被其它窗口完全遮挡（或全屏独占/演示模式）。
    // 为真时 UpdateIdleWatchdog 无条件停看门狗、TickIdle 直接早退。
    bool  m_occluded      = false;
    // P1-6：本次遮挡是【我们】主动 Show(false) 把窗口收起来释放 DComp 合成的。
    // 只有它为真，解除遮挡时才 Show(true) —— 否则会把 autoHide 自己的隐藏态
    // 误"恢复"成可见（用户会看到 dock 在任意窗口移动后无故弹出）。
    bool  m_occlusionHidWindow = false;
    float m_showCountdown = 0.0f;      // >0 时显示延迟倒计时（秒）
    float m_hideCountdown = 0.0f;      // >0 时隐藏延迟倒计时（秒）
    bool  m_probeHoverPending = false; // 显示延迟到期后需执行 Show()
    HANDLE m_watchdogHandle = nullptr; // 低频率看门狗定时器（探测光标 + 倒计时）
    LARGE_INTEGER m_lastIdleTime = {};

    // ═══ 右键删除 + 拖拽排序 + 拖拽添加（Step 8）═══
    std::string m_configPath;          // 解析后的配置文件路径（生产持久化目标）
    bool m_forceGdi = false;           // 测试钩子：--force-gdi 强制 GDI 回退
    int   m_dragIndex  = -1;           // 正在拖拽的图标下标（-1=无）
    bool  m_dragging   = false;
    bool  m_dragMoved  = false;        // 拖拽中是否发生明显位移（区分点击/拖拽）
    POINT m_dragStart  = {};
    // 外部文件拖放预览（IDropTarget 进行中）：拖入时在光标槽位插入半透明占位，
    // 其余图标让位；被拖（占位）图标跟随光标。落盘前转正 / 离开撤销。
    bool  m_externalDragActive = false;  // 预览进行中
    int   m_externalDragStart  = -1;     // 预览块起始下标（连续 m_externalDragCount 个）
    int   m_externalDragCount  = 0;      // 预览占位数量
    int   m_externalDragFloat  = -1;     // 跟随光标浮动的占位下标（-1=不浮动）
    Microsoft::WRL::ComPtr<IDropTarget> m_dropTarget;  // 文件拖放目标（仅 Windowed）
    void RebuildIcons(bool persist);   // 重建布局/弹簧/渲染/窗口并（可选）持久化
    // #3/#4 同步当前边图标集（含共享存储），供增删/重排/切换边调用；单引擎下同步顶层 sharedIcons
    void SyncCurrentEdgeIcons();

    bool m_initialized = false;

    // P1-7：换边/边开关串行化锁（杜绝旧/新引擎短暂同边竞态）。SetDockPosition 与
    // SetEdgeEnabled 均受此锁保护；内部实现走 ApplyDockPosition 避免同一锁重入死锁。
    std::mutex m_edgeMutex;

    // 多实例（DockManager）持久化协作：共享图标存储 + 合并后统一落盘回调（#3 独立存储）
    std::function<void(const AppConfig&)> m_persistCb;
    std::shared_ptr<std::array<std::vector<IconEntry>, 4>> m_sharedEdgeIcons;   // 所有边的图标集（共享）
    std::shared_ptr<std::vector<IconEntry>> m_sharedSharedIcons;                // 共享默认图标集（共享）

    // 性能验收累计器（微秒 / 帧数）
    double m_perfSpringUs = 0.0;
    double m_perfLayoutUs = 0.0;
    long long m_perfFrames = 0;
    bool m_comInitialized = false;   // 本层是否负责 CoInitializeEx（Shutdown 时对应 CoUninitialize）
    std::wstring m_exeDir;           // exe 所在目录（带结尾反斜杠），用于解析相对资源路径
    bool        m_dbg = false;       // 调试日志开关（OPEN_DOCK_DEBUG 环境变量置位，诊断拖放预览用）

    // 系统托盘
    NOTIFYICONDATAW m_nid = {};
    bool m_trayAdded = false;
    bool m_trayIconOwned = false;   // 自定义托盘图标句柄（LoadTrayIcon）需 DestroyIcon

    static constexpr float ENTRY_STAGGER_SEC = 0.03f;   // 入场级联延迟 30ms/图标
    static constexpr float BOUNCE_RESET_SEC  = 0.10f;   // 弹跳回落延迟
};
