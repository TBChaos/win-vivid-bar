// src/app/IconSetManager.h
// DockEngine 子模块：图标集增删改 / 持久化 / 弹簧目标 / 布局查询 / 放置与边配置 / 性能验收
// 方法体见 IconSetManager.cpp；所有 DockEngine 成员经 m_owner-> 访问（friend）。
#pragma once
#include "DockEngine.h"   // IconEntry / IconLayout / AppConfig 等类型

// 子模块经 m_owner（DockEngine*）反查宿主私有成员，故 DockEngine 需声明本类为 friend。
class IconSetManager {
public:
    explicit IconSetManager(DockEngine* owner) : m_owner(owner) {}
    DockEngine* m_owner;

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

    // ═══ 右键删除 + 拖拽排序 + 拖拽添加（Step 8）═══
    std::wstring GetIconPath(int index) const;
    std::wstring GetIconName(int index) const;
    int  GetIconTextureCount() const;    // 已成功加载的图标纹理数（调试/验收用）
    bool RemoveIcon(int index, bool persist);    // 右键删除
    bool ReorderIcon(int from, int to, bool persist); // 拖拽排序
    // 拖拽过程轻量重排（不落盘 / 不重载纹理 / 不重定位窗口）：仅数据重排 + 视觉轻量重排，
    // 复用已解码位图消除闪烁；内部拖动 / 外部拖入共用同一逻辑。
    bool ReorderIconsDuringDrag(int from, int to);
    // 拖拽过程轻量视觉重排：复用已解码位图按 path 重排视觉树（不重新解码 / 不重定位窗口 /
    // 不重建背景），并重启动画循环；供 ReorderIconsDuringDrag 与 MoveExternalDropPreview 共用。
    void RelayoutDuringDrag();
    bool AddIcon(const std::wstring& path, const std::wstring& name, bool persist,
                 int insertAt = -1); // 拖拽添加；insertAt>=0 插入到指定下标（-1 追加末尾）
    void AddIconFromDrop(const std::wstring& path, int insertAt = -1);  // IDropTarget 回调
    void PersistConfigTo(const std::string& path) const; // 写入当前配置（验证用）

    // ═══ 弹簧目标设置 ═══
    void ApplyEntryTargets(int iconIndex);   // 入场（单图标，级联用）
    void ApplyHoverTargets(float mouseXCentered);   // 鱼眼放大
    void ApplyRestTargets();                        // 全部回归静息
    bool AnyScaleElevated() const;                  // #1 看门狗：是否有图标处于放大态
    bool IsFisheyeEnabled() const;                  // #N 当前边是否启用鱼眼放大
    void ApplyExitTargets();
    void TriggerBounce(int iconIndex);

    // P0-1/2/3：静息布局缓存 + 稳定弹簧绑定增量重建
    const std::vector<IconLayout>& EnsureRestLayout() const;   // 脏标记缓存（零弹簧热路径）
    void ReconcileSprings(const std::vector<IconEntry>& newIcons, float initScaleOpacity); // 增量重建

    // ═══ 图标集重建 / 持久化 ═══
    void RebuildIcons(bool persist);   // 重建布局/弹簧/渲染/窗口并（可选）持久化
    void SyncCurrentEdgeIcons();       // #3/#4 同步当前边图标集（含共享存储）
    void PersistConfig() const;        // 统一持久化入口（优先回调）
    void SetConfigPath(const std::string& path);
    void SetDockBarVisible(bool visible);          // #N 切换 Dock 底座背景条显隐
    void SetPersistCallback(std::function<void(const AppConfig&)> cb);
    void SetSharedIcons(std::shared_ptr<std::array<std::vector<IconEntry>, 4>> edges,
                        std::shared_ptr<std::vector<IconEntry>> shared);

    // ═══ 开机自启动 + 位置微调 + 图层 Z 序（Step 10）═══
    void ApplyPlacement();               // 依配置(position/offset/monitor/zOrder)定位窗口
    int  GetWindowZOrder() const;        // 当前 Z 序（1=topmost 0=normal -1=bottom）
    RECT GetDockScreenRect() const;      // 基础 Dock 条屏幕矩形（验证用）
    void SetPlacementOverride(int edgeOffset, int centerOffset, int zOrder);
    void SetDockPosition(DockPosition pos);
    void SetEdgeEnabled(DockPosition edge, bool enabled);
    void SetIconSize(float size);                 // 大小：小/中/大
    void SetBackgroundOpacity(float opacity);      // 透明度（背景条）
    void SetZOrder(int zOrder);                    // #5 图层位置：1=前 0=正常 -1=后
    bool AddAppViaDialog();                        // 文件对话框添加应用
    bool AddFolderViaDialog();                     // #2 文件夹选择对话框添加文件夹

    // ═══ Step 12：放大溢出留白（依停靠边 + maxScale 计算四边留白）═══
    void ComputeInsets(int& left, int& top, int& right, int& bottom) const;

    // P0-4：角格边长 = min(dockWidth, dockHeight)（相邻两带法向厚度交叠正方形，取较小带厚）

    // Step 14 验证辅助
    bool GetIconScreenCenter(int index, float& screenX, float& screenY) const;
    bool GetIconCurrentScreenCenter(int index, float& screenX, float& screenY) const;
    bool GetLayout(int index, float& mainX, float& crossY) const;

    // 计算拖拽释放时的插入位（原始图标索引序号）
    int  ComputeDragInsertIndex(POINT pt);
};
