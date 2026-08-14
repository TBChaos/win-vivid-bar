// src/app/ConfigManager.cpp
#include "ConfigManager.h"
#include "../utils/JsonUtil.h"
#include "../utils/PathUtil.h"
#include "../platform/AutoStart.h"
#include <fstream>
#include <sstream>

// 注：原 SameIconList 仅服务于「每边排列与共享集不一致时打一条诊断日志」，
// 随诊断日志一并移除。

bool ConfigManager::Load(const std::string& path, AppConfig& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string json = ss.str();
    in.close();

    float f = 0.0f; bool b = false; std::string s;

    // ═══ dock ═══
    if (JsonUtil::FindNumber(json, "iconSize", f))        out.dock.baseIconSize = f;
    if (JsonUtil::FindNumber(json, "maxScale", f))        out.dock.maxScale = f;
    if (JsonUtil::FindNumber(json, "iconSpacing", f))     out.dock.iconSpacing = f;
    if (JsonUtil::FindNumber(json, "magnifyRadius", f))   out.dock.magnifyRadius = f;
    if (JsonUtil::FindNumber(json, "bounceAmplitude", f)) out.dock.bounceAmplitude = f;
    if (JsonUtil::FindString(json, "position", s)) {
        if (s == "top")    out.dock.position = DockPosition::Top;
        else if (s == "left")  out.dock.position = DockPosition::Left;
        else if (s == "right") out.dock.position = DockPosition::Right;
        else                   out.dock.position = DockPosition::Bottom;
    }
    // #3：四边吸附/感应区独立开关（缺省键时保持默认 true）
    if (JsonUtil::FindBool(json, "edgeTop", b))    out.dock.edgeTop    = b;
    if (JsonUtil::FindBool(json, "edgeBottom", b)) out.dock.edgeBottom = b;
    if (JsonUtil::FindBool(json, "edgeLeft", b))   out.dock.edgeLeft   = b;
    if (JsonUtil::FindBool(json, "edgeRight", b))  out.dock.edgeRight  = b;
    // #N 四边独立启用开关：优先读取顶层 "edgeEnabled": [bottom,top,left,right]（4 布尔）；
    // 缺省则从 dock.edgeTop/Bottom/Left/Right 映射（向后兼容既有 config.json）。
    {
        std::array<bool, 4> arr = { out.dock.edgeBottom, out.dock.edgeTop,
                                    out.dock.edgeLeft,   out.dock.edgeRight };
        std::vector<bool> ea;
        if (JsonUtil::FindBoolArray(json, "edgeEnabled", ea, 4)) {
            for (size_t i = 0; i < ea.size() && i < arr.size(); ++i) arr[i] = ea[i];
        }
        out.edgeEnabled = arr;
    }
    // #N：四边鱼眼放大独立开关（缺省键时保持默认 true）
    if (JsonUtil::FindBool(json, "fisheyeTop", b))    out.dock.fisheyeTop    = b;
    if (JsonUtil::FindBool(json, "fisheyeBottom", b)) out.dock.fisheyeBottom = b;
    if (JsonUtil::FindBool(json, "fisheyeLeft", b))   out.dock.fisheyeLeft   = b;
    if (JsonUtil::FindBool(json, "fisheyeRight", b))  out.dock.fisheyeRight  = b;

    // ═══ animation ═══
    if (JsonUtil::FindNumber(json, "hoverStiffness", f))  out.hoverParams.stiffness = f;
    if (JsonUtil::FindNumber(json, "hoverDamping", f))    out.hoverParams.damping = f;
    if (JsonUtil::FindNumber(json, "bounceStiffness", f)) out.bounceParams.stiffness = f;
    if (JsonUtil::FindNumber(json, "bounceDamping", f))   out.bounceParams.damping = f;
    if (JsonUtil::FindNumber(json, "entryStiffness", f))  out.entryParams.stiffness = f;
    if (JsonUtil::FindNumber(json, "entryDamping", f))    out.entryParams.damping = f;

    // ═══ appearance ═══
    if (JsonUtil::FindNumber(json, "backgroundOpacity", f)) out.backgroundOpacity = f;
    if (JsonUtil::FindBool(json, "backgroundBlur", b))      out.backgroundBlur = b;
    if (JsonUtil::FindNumber(json, "cornerRadius", f))      out.cornerRadius = f;
    if (JsonUtil::FindBool(json, "shadowEnabled", b))       out.shadowEnabled = b;
    if (JsonUtil::FindBool(json, "tooltipEnabled", b))      out.tooltipEnabled = b;
    if (JsonUtil::FindBool(json, "dockBarVisible", b))       out.dockBarVisible = b;

    // ═══ display ═══
    if (JsonUtil::FindNumber(json, "monitor", f))           out.monitorIndex = (int)f;

    // ═══ autohide（Step 7）═══
    if (JsonUtil::FindBool(json, "autoHide", b))            out.autoHide    = b;
    if (JsonUtil::FindNumber(json, "showDelay", f))         out.showDelayMs = (int)f;
    if (JsonUtil::FindNumber(json, "hideDelay", f))         out.hideDelayMs = (int)f;

    // ═══ behavior / position（Step 10）═══
    if (JsonUtil::FindBool(json, "autoStart", b))           out.autoStart    = b;
    if (JsonUtil::FindNumber(json, "edgeOffset", f))        out.edgeOffset   = (int)f;
    if (JsonUtil::FindNumber(json, "centerOffset", f))      out.centerOffset = (int)f;
    if (JsonUtil::FindString(json, "zOrder", s)) {          // "top" / "normal" / "bottom"
        if (s == "bottom")      out.zOrder = -1;
        else if (s == "normal") out.zOrder = 0;
        else                    out.zOrder = 1;
    }

    // ═══ icons 数组（共享默认，逐对象扫描 path/index/name）═══
    out.icons.clear();
    {
        const std::string iconsArr = JsonUtil::ExtractArray(json, "icons");
        if (!iconsArr.empty()) out.icons = ParseIconArray(iconsArr, 0);
    }
    out.sharedIcons = out.icons;                       // #4 共享默认
    for (int e = 0; e < 4; ++e) out.edgeIcons[e] = out.sharedIcons;  // #4 每边默认=共享

    // ═══ edges 对象（#4 每边独立图标集，可选）═══
    // 结构： "edges": { "top":[...], "bottom":[...], "left":[...], "right":[...] }
    //
    // D8 修复（用户报障「重启后图标排列变回默认」的真根因）：
    //   旧代码用 json.find('}', objStart) 取 edges 对象末尾 —— 这是【非配对】扫描，
    //   找到的是 edges 里【第一个图标对象】的右花括号，edgesJson 被截成 95 字符残片：
    //       { "left": [ { "path": "...cmd.exe", "name": "命令提示符", "index": 0 }
    //   残片里 "top"/"bottom"/"right" 三个 key 根本不存在，"left" 的 '[' 也没有配对的 ']'，
    //   ParseIconArray 拿到 arrEnd==npos 静默返回空 vector → 四边全空 → DockEngine 回退
    //   sharedIcons → 每条边的独立排列 100% 丢失。
    //   现改用 JsonUtil::ExtractObject/ExtractArray 的深度配对扫描（且跳过字符串字面量
    //   内的括号，Windows 路径里的反斜杠转义已正确处理）。
    const std::string edgesJson = JsonUtil::ExtractObject(json, "edges");
    if (!edgesJson.empty()) {
        struct { const char* key; DockPosition pos; } map[] = {
            { "top", DockPosition::Top }, { "bottom", DockPosition::Bottom },
            { "left", DockPosition::Left }, { "right", DockPosition::Right } };
        for (auto& m : map) {
            const std::string arr = JsonUtil::ExtractArray(edgesJson, m.key);
            if (arr.empty()) continue;          // 该边未配置 → 保留共享默认
            out.edgeIcons[(int)m.pos] = ParseIconArray(arr, 0);
        }
    }
    if (!out.icons.empty()) {
        out.dock.iconCount = (int)out.icons.size();
    }

    return true;
}

bool ConfigManager::SaveDefault(const std::string& path) {
    std::ofstream outFile(path, std::ios::binary);
    if (!outFile.is_open()) return false;
    outFile <<
R"({
  "dock": {
    "position": "bottom",
    "edgeTop": true,
    "edgeBottom": true,
    "edgeLeft": true,
    "edgeRight": true,
    "fisheyeTop": true,
    "fisheyeBottom": true,
    "fisheyeLeft": true,
    "fisheyeRight": true,
    "iconSize": 48,
    "maxScale": 2.0,
    "iconSpacing": 8,
    "magnifyRadius": 3,
    "bounceAmplitude": 20,
    "autoHide": false,
    "showDelay": 0,
    "hideDelay": 0
  },
  "animation": {
    "hoverStiffness": 300,
    "hoverDamping": 20,
    "bounceStiffness": 180,
    "bounceDamping": 12,
    "entryStiffness": 200,
    "entryDamping": 18
  },
  "appearance": {
    "backgroundOpacity": 0.6,
    "backgroundBlur": false,
    "cornerRadius": 16,
    "shadowEnabled": true,
    "tooltipEnabled": true
  },
  "display": {
    "monitor": 0
  },
  "autoStart": false,
  "edgeOffset": 0,
  "centerOffset": 0,
  "zOrder": "top",
  "edgeEnabled": [true, true, true, true],
  "icons": []
}
)";
    outFile.close();
    return true;
}

// JSON 取值已统一至 JsonUtil（src/utils/JsonUtil.cpp），此处不再保留重复实现。


// #4：从 JSON 中某个 "[" 起始位置扫描图标对象数组（委托 JsonUtil 解析，UTF-8→宽串）
std::vector<IconEntry> ConfigManager::ParseIconArray(const std::string& json, size_t arrStart) {
    std::vector<IconEntry> out;
    const std::vector<JsonUtil::JsonIcon> raw = JsonUtil::ParseIconArray(json, arrStart);
    for (const JsonUtil::JsonIcon& j : raw) {
        IconEntry entry;
        entry.path       = PathUtil::Utf8ToWide(j.path);
        entry.name       = PathUtil::Utf8ToWide(j.name);
        entry.args       = PathUtil::Utf8ToWide(j.args);
        entry.workingDir = PathUtil::Utf8ToWide(j.workingDir);
        entry.index      = j.index;
        if (!entry.path.empty()) out.push_back(entry);
    }
    return out;
}

// UTF-8→宽串转换已统一至 PathUtil::Utf8ToWide（src/utils/PathUtil.h）。
// 写出转义统一至 JsonUtil::JsonEscape(PathUtil::WideToUtf8(...))（src/utils/JsonUtil.h）。

bool ConfigManager::SaveConfig(const AppConfig& cfg, const std::string& path) {
    auto f = [](float v) { char b[64]; snprintf(b, sizeof(b), "%.4g", (double)v); return std::string(b); };

    std::string s;
    s += "{\n";
    // dock
    s += "  \"dock\": {\n";
    s += "    \"position\": " + JsonUtil::JsonEscape(PathUtil::WideToUtf8(
            cfg.dock.position == DockPosition::Top ? L"top" :
            cfg.dock.position == DockPosition::Left ? L"left" :
            cfg.dock.position == DockPosition::Right ? L"right" : L"bottom")) + ",\n";
    // dock.edge* 由权威开关 edgeEnabled 镜像写出（向后兼容旧读取端）
    s += "    \"edgeTop\": " + std::string(cfg.edgeEnabled[(int)DockPosition::Top] ? "true" : "false") + ",\n";
    s += "    \"edgeBottom\": " + std::string(cfg.edgeEnabled[(int)DockPosition::Bottom] ? "true" : "false") + ",\n";
    s += "    \"edgeLeft\": " + std::string(cfg.edgeEnabled[(int)DockPosition::Left] ? "true" : "false") + ",\n";
    s += "    \"edgeRight\": " + std::string(cfg.edgeEnabled[(int)DockPosition::Right] ? "true" : "false") + ",\n";
    s += "    \"fisheyeTop\": " + std::string(cfg.dock.fisheyeTop ? "true" : "false") + ",\n";
    s += "    \"fisheyeBottom\": " + std::string(cfg.dock.fisheyeBottom ? "true" : "false") + ",\n";
    s += "    \"fisheyeLeft\": " + std::string(cfg.dock.fisheyeLeft ? "true" : "false") + ",\n";
    s += "    \"fisheyeRight\": " + std::string(cfg.dock.fisheyeRight ? "true" : "false") + ",\n";
    s += "    \"iconSize\": " + f(cfg.dock.baseIconSize) + ",\n";
    s += "    \"maxScale\": " + f(cfg.dock.maxScale) + ",\n";
    s += "    \"iconSpacing\": " + f(cfg.dock.iconSpacing) + ",\n";
    s += "    \"magnifyRadius\": " + f(cfg.dock.magnifyRadius) + ",\n";
    s += "    \"bounceAmplitude\": " + f(cfg.dock.bounceAmplitude) + "\n";
    s += "  },\n";
    // animation
    s += "  \"animation\": {\n";
    s += "    \"hoverStiffness\": " + f(cfg.hoverParams.stiffness) + ",\n";
    s += "    \"hoverDamping\": " + f(cfg.hoverParams.damping) + ",\n";
    s += "    \"bounceStiffness\": " + f(cfg.bounceParams.stiffness) + ",\n";
    s += "    \"bounceDamping\": " + f(cfg.bounceParams.damping) + ",\n";
    s += "    \"entryStiffness\": " + f(cfg.entryParams.stiffness) + ",\n";
    s += "    \"entryDamping\": " + f(cfg.entryParams.damping) + "\n";
    s += "  },\n";
    // appearance
    s += "  \"appearance\": {\n";
    s += "    \"backgroundOpacity\": " + f(cfg.backgroundOpacity) + ",\n";
    s += "    \"backgroundBlur\": " + std::string(cfg.backgroundBlur ? "true" : "false") + ",\n";
    s += "    \"cornerRadius\": " + f(cfg.cornerRadius) + ",\n";
    s += "    \"shadowEnabled\": " + std::string(cfg.shadowEnabled ? "true" : "false") + ",\n";
    s += "    \"tooltipEnabled\": " + std::string(cfg.tooltipEnabled ? "true" : "false") + ",\n";
    s += "    \"dockBarVisible\": " + std::string(cfg.dockBarVisible ? "true" : "false") + "\n";
    s += "  },\n";
    // display + autohide
    s += "  \"display\": { \"monitor\": " + std::to_string(cfg.monitorIndex) + " },\n";
    s += "  \"autoHide\": " + std::string(cfg.autoHide ? "true" : "false") + ",\n";
    s += "  \"showDelay\": " + std::to_string(cfg.showDelayMs) + ",\n";
    s += "  \"hideDelay\": " + std::to_string(cfg.hideDelayMs) + ",\n";
    // behavior / position（Step 10）
    s += "  \"autoStart\": " + std::string(cfg.autoStart ? "true" : "false") + ",\n";
    s += "  \"edgeOffset\": " + std::to_string(cfg.edgeOffset) + ",\n";
    s += "  \"centerOffset\": " + std::to_string(cfg.centerOffset) + ",\n";
    s += "  \"zOrder\": " + JsonUtil::JsonEscape(PathUtil::WideToUtf8(
            cfg.zOrder < 0 ? L"bottom" : cfg.zOrder == 0 ? L"normal" : L"top")) + ",\n";
    // #N 四边独立启用开关（顶层数组，顺序=DockPosition: bottom,top,left,right；不强制至少一条）
    s += "  \"edgeEnabled\": ["
       + std::string(cfg.edgeEnabled[0] ? "true" : "false") + ", "
       + std::string(cfg.edgeEnabled[1] ? "true" : "false") + ", "
       + std::string(cfg.edgeEnabled[2] ? "true" : "false") + ", "
       + std::string(cfg.edgeEnabled[3] ? "true" : "false") + "],\n";
    // icons（#4：顶层 icons = 共享默认；每边独立集写入 edges）
    s += "  \"icons\": [\n";
    for (size_t i = 0; i < cfg.sharedIcons.size(); ++i) {
        const IconEntry& e = cfg.sharedIcons[i];
        if (e.preview) continue;   // 拖放预览占位不落盘
        s += "    { \"path\": " + JsonUtil::JsonEscape(PathUtil::WideToUtf8(e.path));
        s += ", \"name\": " + JsonUtil::JsonEscape(PathUtil::WideToUtf8(e.name));
        s += ", \"index\": " + std::to_string(e.index);
        if (!e.args.empty())       s += ", \"args\": " + JsonUtil::JsonEscape(PathUtil::WideToUtf8(e.args));
        if (!e.workingDir.empty()) s += ", \"workingDir\": " + JsonUtil::JsonEscape(PathUtil::WideToUtf8(e.workingDir));
        s += " }";
        if (i + 1 < cfg.sharedIcons.size()) s += ",";
        s += "\n";
    }
    s += "  ]";
    // #4：每边独立图标集 —— 【无条件全写四边】。
    // 旧实现仅在 !IconsEqual(该边, sharedIcons) 时才写出：某边恰好与共享集相同就整块省略，
    // 下次加载时该边落回"未配置"分支 → 回退共享集。这让"相同"与"未配置"两种语义混在一起，
    // 是 D8 之外第二条丢排列的路径，且极难排查。现在四边一律写出（含空数组）。
    {
        static const char* edgeNames[4] = { "bottom", "top", "left", "right" };
        s += ",\n  \"edges\": {\n";
        for (int e = 0; e < 4; ++e) {
            s += "    \"" + std::string(edgeNames[e]) + "\": [\n";
            const auto& arr = cfg.edgeIcons[e];
            for (size_t i = 0; i < arr.size(); ++i) {
                const IconEntry& e2 = arr[i];
                if (e2.preview) continue;   // 拖放预览占位不落盘
                s += "      { \"path\": " + JsonUtil::JsonEscape(PathUtil::WideToUtf8(e2.path));
                s += ", \"name\": " + JsonUtil::JsonEscape(PathUtil::WideToUtf8(e2.name));
                s += ", \"index\": " + std::to_string(e2.index);
                if (!e2.args.empty())       s += ", \"args\": " + JsonUtil::JsonEscape(PathUtil::WideToUtf8(e2.args));
                if (!e2.workingDir.empty()) s += ", \"workingDir\": " + JsonUtil::JsonEscape(PathUtil::WideToUtf8(e2.workingDir));
                s += " }";
                if (i + 1 < arr.size()) s += ",";
                s += "\n";
            }
            s += "    ]";
            if (e < 3) s += ",";
            s += "\n";
        }
        s += "  }";
    }
    s += "\n}\n";

    return WriteFileAtomic(path, s);
}

// 原子写：先写 <path>.tmp，flush + 校验流状态，再用 MoveFileExW 替换目标。
// 旧实现直接 ofstream 截断目标文件 —— 写到一半崩溃/断电就把配置写坏，而 Load 遇到
// 损坏文件只会"找不到 key → 用默认值"，静默把用户配置换成出厂默认。
// MOVEFILE_WRITE_THROUGH 保证替换动作在返回前落到磁盘，避免元数据滞留在缓存里。
bool ConfigManager::WriteFileAtomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream outFile(tmp, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open()) {
            return false;
        }
        outFile << content;
        outFile.flush();
        if (!outFile.good()) {
            outFile.close();
            remove(tmp.c_str());
            return false;
        }
    }
    const std::wstring wTmp = PathUtil::Utf8ToWide(tmp);
    const std::wstring wDst = PathUtil::Utf8ToWide(path);
    if (!MoveFileExW(wTmp.c_str(), wDst.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
// Step 10：开机自启动 —— 实现已搬到 src/platform/AutoStart.{h,cpp}
// （注册表访问是平台设施，不属于配置序列化职责；且原实现每边调一次、无校验自愈）。
// 此处仅保留薄转发，供既有调用点/测试过渡；新代码一律直接用 AutoStart::*。
// ═══════════════════════════════════════════════════════════
bool ConfigManager::ApplyAutoStart(bool enable, const std::wstring& exePath) {
    return enable ? AutoStart::Enable(exePath) : AutoStart::Disable();
}

bool ConfigManager::QueryAutoStart(std::wstring* outPath) {
    const AutoStart::Query q = AutoStart::Read();
    const bool on = (q.status == AutoStart::Status::EnabledCurrent ||
                     q.status == AutoStart::Status::EnabledStale);
    if (on && outPath) *outPath = q.rawValue;
    return on;
}
