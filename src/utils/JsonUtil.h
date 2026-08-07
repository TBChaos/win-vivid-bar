// src/utils/JsonUtil.h
// 统一 JSON 解析（零第三方依赖手写实现，合并自 ConfigManager 与 EdgeConfig 的两份重复实现）
//
// 设计要点：
//   * 全部为 static 纯函数，不依赖任何 Win32 / COM，可 Headless 单测。
//   * 仅支持本项目 config.json 的 schema（扁平 key 查找 + 对象/数组括号配对提取）。
//   * IconEntry 定义在 app/ConfigManager.h，为避免 utils 反向依赖 app 层，
//     ParseIconArray 返回 JsonIcon（字段均为 std::string），由调用方负责 UTF-8→宽串转换。
// 关联：优化架构设计 §4（P1-1）。
#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct JsonUtil {
    // —— 标量取值（单次从左到右扫描，命中即停；不存在返回 false）——
    static bool FindNumber(const std::string& json, const std::string& key, float& out);
    static bool FindBool(const std::string& json, const std::string& key, bool& out);
    static bool FindString(const std::string& json, const std::string& key, std::string& out);

    // —— 对象/数组提取（括号配对，支持嵌套）——
    static std::string ExtractObject(const std::string& json, const std::string& key); // "{...}"
    static std::string ExtractArray(const std::string& json, const std::string& key);  // "[...]"

    // —— 布尔数组（"edgeEnabled":[true,false,true,false]）——
    static bool FindBoolArray(const std::string& json, const std::string& key,
                              std::vector<bool>& out, size_t maxCount = 4);

    // —— 图标数组（解析 path/name/args/workingDir/index）——
    struct JsonIcon {
        std::string path;        // 图标/程序路径（UTF-8）
        std::string name;        // 显示名（UTF-8）
        std::string args;        // 启动参数（UTF-8）
        std::string workingDir;  // 工作目录（UTF-8）
        int         index = 0;   // 资源内图标序号
    };
    static std::vector<JsonIcon> ParseIconArray(const std::string& json, size_t arrStart);

    // —— 字符串转义（UTF-8 / 宽串 → JSON 字符串字面量）——
    static std::string  JsonEscape(const std::string& s);
    static std::wstring JsonEscape(const std::wstring& w);
};
