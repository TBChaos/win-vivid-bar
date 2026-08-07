// src/core/EdgeConfig.h
// 统一四边配置（类内成员初始化 + 四区域继承加载）
// 设计要点：
//   * EdgeConfig：每个边的配置，全部用类内成员初始化给出默认值
//     （enabled/iconSize/iconSpacing/padding/maxScale/fisheye/可选 icons）。
//     四个边共用同一份 schema，实现"一致表现形式"。
//   * DockConfigStore：从 res/config.json 加载 —— 读取一个 defaults 对象 +
//     一个 edges 对象（含 bottom/top/left/right 四个区域，每区只写覆盖项），
//     逐边继承 defaults（区域缺省字段回退 defaults）。输出 std::array<EdgeConfig,4>
//     （索引=DockPosition）。沿用现有极简 JSON 取值器风格（不引入第三方库）。
// ══════════════════════════════════════════════════════════════════════
#pragma once
#include "../Common.h"
#include "../app/ConfigManager.h"   // IconEntry 定义（EdgeConfig.icons 使用）
#include <array>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "../utils/JsonUtil.h"

// ══════════════════════════════════════════════════════════════════════
// 单条边配置：类内成员初始化给出默认值（统一表现形式）
// ══════════════════════════════════════════════════════════════════════
struct EdgeConfig {
    // 注意：该边「是否启用」不再由 EdgeConfig 承载（P0-6）。
    // 唯一真源为 AppConfig.edgeEnabled[4]（DockEngine::IsEdgeEnabled 全量门控），
    // 由 DockManager 托盘菜单/右键菜单统一读写，避免双真源误导。
    float iconSize    = DockConstants::DEFAULT_ICON_SIZE;           // 图标基础尺寸
    float iconSpacing = DockConstants::DEFAULT_ICON_SPACING;        // 图标间距
    float padding     = DockConstants::DEFAULT_PADDING;             // 停靠边内边距
    float maxScale    = DockConstants::DEFAULT_MAX_SCALE;           // 鱼眼最大缩放
    bool  fisheye     = true;                                       // 该边是否启用鱼眼放大
    std::vector<IconEntry> icons;                                   // 该边独立图标集（可选）
};

// ══════════════════════════════════════════════════════════════════════
// 四边配置存储：从 JSON 的 defaults + edges(四区域) 加载，逐边继承 defaults
// ══════════════════════════════════════════════════════════════════════
struct DockConfigStore {
    // 从 JSON 文件加载：defaults 作为基准，edges 四区域逐区继承并覆盖。
    // 输出索引=DockPosition 的数组（Bottom/Top/Left/Right）。
    // 文件缺失/解析失败返回 false（调用方保留默认 EdgeConfig）。
    static bool Load(const std::string& path, std::array<EdgeConfig, 4>& outEdges);

private:
    // JSON 取值已统一至 JsonUtil（src/utils/JsonUtil.h），此处不再保留重复实现。
    // 解析单个边对象子串中的覆盖项（从父 EdgeConfig 继承后覆盖，使用 JsonUtil::Find*）
    static inline void ParseEdge(const std::string& objJson, EdgeConfig& edge) {
        if (objJson.empty()) return;
        float f = 0.0f; bool b = false;
        if (JsonUtil::FindNumber(objJson, "iconSize",    f)) edge.iconSize    = f;
        if (JsonUtil::FindNumber(objJson, "iconSpacing", f)) edge.iconSpacing = f;
        if (JsonUtil::FindNumber(objJson, "padding",     f)) edge.padding     = f;
        if (JsonUtil::FindNumber(objJson, "maxScale",    f)) edge.maxScale    = f;
        if (JsonUtil::FindBool(objJson,   "fisheye",     b)) edge.fisheye     = b;
    }
};

inline bool DockConfigStore::Load(const std::string& path,
                                  std::array<EdgeConfig, 4>& outEdges) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string json = ss.str();
    in.close();

    // 1) defaults：从默认 EdgeConfig 起，应用覆盖项
    EdgeConfig defaults;
    std::string defObj = JsonUtil::ExtractObject(json, "defaults");
    ParseEdge(defObj, defaults);

    // 2) edges 四区域：从 defaults 继承，逐区覆盖（每区只写覆盖项）
    std::string edgesObj = JsonUtil::ExtractObject(json, "edges");
    struct { const char* key; DockPosition pos; } map[] = {
        { "bottom", DockPosition::Bottom }, { "top", DockPosition::Top },
        { "left",   DockPosition::Left },   { "right", DockPosition::Right } };
    for (auto& m : map) {
        outEdges[(int)m.pos] = defaults;   // 继承 defaults
        if (!edgesObj.empty()) {
            std::string keyPat = "\"" + std::string(m.key) + "\"";
            size_t kp = edgesObj.find(keyPat);
            if (kp != std::string::npos) {
                size_t ob = edgesObj.find('{', kp);
                if (ob != std::string::npos) {
                    int depth = 0;
                    for (size_t i = ob; i < edgesObj.size(); ++i) {
                        if (edgesObj[i] == '{') ++depth;
                        else if (edgesObj[i] == '}') {
                            --depth;
                            if (depth == 0) {
                                std::string region = edgesObj.substr(ob, i - ob + 1);
                                ParseEdge(region, outEdges[(int)m.pos]);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}
