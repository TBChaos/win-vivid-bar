// src/app/ConfigManager.h
// 配置管理 — 加载/解析 config.json（设计参考：详细设计说明 §七）
// 说明：内置极简容错 JSON 取值器（仅支持本项目 schema），零第三方依赖
#pragma once
#include "../Common.h"
#include "../core/LayoutEngine.h"
#include <array>

struct IconEntry {
    std::wstring path;       // 图标/程序路径（同时作为启动目标）
    int          index = 0;  // 资源内图标序号（EXE/DLL）
    std::wstring name;       // 显示名（Tooltip）
    std::wstring args;       // 启动参数（可选）
    std::wstring workingDir; // 工作目录（可选，相对路径按 exe 目录解析）
    bool         preview = false;  // 拖放预览占位（不落盘；落盘前必转正或移除）
};

// ═══ 拖拽排序的纯数组重排（与渲染/窗口/COM 无关，可独立单测）═══
// from / to 均为【原始数组】下标语义：
//   - from：被拖拽图标原下标；
//   - to  ：释放点在原始数组中的「插入位」（0=最前，n=最后/附加到末尾）。
// 删除 from 后，对剩余数组做插入位偏移修正：
//   dest = (from < to) ? to - 1 : to
// 关键修复：to 允许取到 n（拖到【最末尾】），旧实现把 to clamp 到 n-1 导致
// 「拖到 Dock 末尾」永远落回倒数第二，顺序更新功能在末端失效。
// 返回 true 表示顺序确实发生了改变（from==to 或越界返回 false = 无操作）。
inline bool ReorderIconEntries(std::vector<IconEntry>& v, int from, int to) {
    const int n = (int)v.size();
    if (from < 0 || from >= n) return false;
    if (to < 0) to = 0;
    if (to > n) to = n;        // 允许 n：附加到末尾（修复末端插入缺陷）
    if (from == to) return false;
    IconEntry e = v[from];
    v.erase(v.begin() + from);
    int dest = (from < to) ? to - 1 : to;   // 删除后索引偏移修正
    if (dest < 0) dest = 0;
    if (dest > (int)v.size()) dest = (int)v.size();
    v.insert(v.begin() + dest, e);
    return true;
}

struct AppConfig {
    DockConfig dock;                       // 布局/物理参数
    // animation
    SpringParams hoverParams  = SpringParams::Hover();
    SpringParams bounceParams = SpringParams::Bounce();
    SpringParams entryParams  = SpringParams::Entry();
    SpringParams restParams   = SpringParams::Rest();   // #4 鱼眼复位专用（快速临界阻尼）
    // appearance
    float backgroundOpacity = 0.6f;
    bool  backgroundBlur    = true;
    float cornerRadius      = 16.0f;
    bool  shadowEnabled     = true;
    bool  tooltipEnabled    = true;
    bool  dockBarVisible    = false;   // #N Dock 底座背景条是否显示（默认隐藏，仅浮出图标）
    // display
    int   monitorIndex      = 0;       // 显示器序号（0=主显示器）
    // autohide（Step 7：自动隐藏 + 空闲鼠标穿透）
    bool  autoHide          = true;    // #7 默认开启边缘感应（隐藏于屏幕边缘，鼠标靠近弹出）
    int   showDelayMs       = 0;       // 靠近边缘后弹出的延迟（ms）
    int   hideDelayMs       = 0;       // 鼠标离开后隐藏的延迟（ms）；0=离开立即隐藏
    // behavior / position（Step 10：开机自启动 + 位置微调 + 图层 Z 序）
    bool  autoStart         = false;   // 随 Windows 启动（HKCU Run 键）
    int   edgeOffset        = 0;       // 距屏幕边缘的偏移（px，垂直于停靠边）
    int   centerOffset      = 0;       // 沿停靠边方向的居中偏移（px，正值向右/向下）
    int   zOrder            = -1;      // 图层：1=总在前面(TOPMOST) 0=正常 -1=总在后面（默认总在后面）
    // icons
    std::vector<IconEntry> icons;          // 当前激活边图标集（运行时镜像，随停靠边切换）
    std::vector<IconEntry> sharedIcons;    // #4 共享默认图标集（序列化顶层 icons；未指定 per-edge 时使用）
    std::array<std::vector<IconEntry>, 4> edgeIcons{};  // #4 每边独立图标集（索引=DockPosition: Bottom/Top/Left/Right）
    // #N 四边独立启用开关（索引=DockPosition: Bottom/Top/Left/Right）；
    // 允许任意组合（含全 false=仅托盘、不显示任何边），不强制至少显示一条边。
    std::array<bool, 4> edgeEnabled = { true, true, true, true };
};

class ConfigManager {
public:
    // 加载失败时返回 false 并保留默认值（错误容忍）
    bool Load(const std::string& path, AppConfig& outConfig);

    // 写出默认配置模板
    bool SaveDefault(const std::string& path);

    // 序列化当前（可能经运行时增删/重排的）配置（Step 8 持久化）
    bool SaveConfig(const AppConfig& cfg, const std::string& path);

    // ═══ Step 10：开机自启动（HKCU\...\Run 注册表）═══
    // enable=true 写入 "openDock"=exePath；false 删除键值。返回是否成功。
    // exePath 为空时自动取当前进程完整路径。
    static bool ApplyAutoStart(bool enable, const std::wstring& exePath = L"");
    // 查询 Run 键中是否已注册 openDock（outPath 可选返回登记的命令行）
    static bool QueryAutoStart(std::wstring* outPath = nullptr);

private:
    // 极简 JSON 工具已统一至 JsonUtil（src/utils/JsonUtil.h）；此处仅保留图标数组解析封装。
    // #4：从 JSON 中某个 "[" 起始位置扫描图标对象数组，返回解析出的图标列表
    static std::vector<IconEntry> ParseIconArray(const std::string& json, size_t arrStart);
    // 原子落盘：临时文件 + MoveFileExW 替换，杜绝"写一半崩溃 → 配置损坏 → 静默回默认"
    static bool WriteFileAtomic(const std::string& path, const std::string& content);
};
