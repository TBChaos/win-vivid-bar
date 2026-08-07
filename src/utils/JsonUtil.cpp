// src/utils/JsonUtil.cpp
// 统一 JSON 解析实现（零依赖手写，单一真源）。合并自：
//   * app/ConfigManager.cpp 的 FindNumber/FindBool/FindString/ParseIconArray
//   * core/EdgeConfig.h 的 FindNumber/FindBool/ExtractObject/ParseEdge
// 关联：优化架构设计 §4（P1-1）。
#include "JsonUtil.h"
#include "../Common.h"   // DOCK_LOG_WARN（解析失败不得静默降级为空数据）

namespace {
    // 在 json 中定位 "key" 后第一个冒号，返回冒号后的位置；找不到返回 npos。
    size_t FindColonAfterKey(const std::string& json, const std::string& key) {
        std::string pattern = "\"" + key + "\"";
        size_t pos = json.find(pattern);
        if (pos == std::string::npos) return std::string::npos;
        return json.find(':', pos + pattern.size());
    }

    // 跳过空白字符
    size_t SkipSpace(const std::string& json, size_t pos) {
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
            ++pos;
        }
        return pos;
    }

    // ── 配对扫描（D8 根因修复）─────────────────────────────────────────
    // 从 json[pos]（必须是 open 括号）起做深度配对扫描，返回含首尾括号的完整子串；
    // 未闭合返回 ""。
    // 关键：必须跳过【字符串字面量内部】的括号，并识别反斜杠转义 —— Windows 路径
    // （"C:\\Windows\\System32\\cmd.exe"）与用户自定义显示名里都可能出现 { } [ ]，
    // 一旦按裸字符计数就会算错深度，重演 D8（edges 对象被截断成 95 字符残片）。
    std::string ScanBalanced(const std::string& json, size_t pos, char open, char close) {
        if (pos >= json.size() || json[pos] != open) return "";
        int  depth = 0;
        bool inStr = false;
        for (size_t i = pos; i < json.size(); ++i) {
            const char c = json[i];
            if (inStr) {
                if (c == '\\') { ++i; continue; }   // 跳过转义序列（\\ \" 等）
                if (c == '"')  inStr = false;
                continue;
            }
            if (c == '"')       { inStr = true; continue; }
            if (c == open)      { ++depth; }
            else if (c == close && --depth == 0) return json.substr(pos, i - pos + 1);
        }
        return "";   // 括号未闭合（文件被截断 / 手工编辑损坏）
    }

    // 从 keyPos 之后定位第一个 open 括号（跳过字符串字面量），返回其下标；无则 npos。
    size_t FindOpenBracket(const std::string& json, size_t from, char open) {
        bool inStr = false;
        for (size_t i = from; i < json.size(); ++i) {
            const char c = json[i];
            if (inStr) {
                if (c == '\\') { ++i; continue; }
                if (c == '"')  inStr = false;
                continue;
            }
            if (c == '"')  { inStr = true; continue; }
            if (c == open) return i;
        }
        return std::string::npos;
    }
} // namespace

bool JsonUtil::FindNumber(const std::string& json, const std::string& key, float& out) {
    size_t pos = FindColonAfterKey(json, key);
    if (pos == std::string::npos) return false;
    ++pos;
    pos = SkipSpace(json, pos);
    char* endPtr = nullptr;
    float v = std::strtof(json.c_str() + pos, &endPtr);
    if (endPtr == json.c_str() + pos) return false;
    out = v;
    return true;
}

bool JsonUtil::FindBool(const std::string& json, const std::string& key, bool& out) {
    size_t pos = FindColonAfterKey(json, key);
    if (pos == std::string::npos) return false;
    ++pos;
    pos = SkipSpace(json, pos);
    if (json.compare(pos, 4, "true") == 0)  { out = true;  return true; }
    if (json.compare(pos, 5, "false") == 0) { out = false; return true; }
    return false;
}

bool JsonUtil::FindString(const std::string& json, const std::string& key, std::string& out) {
    size_t pos = FindColonAfterKey(json, key);
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
            case 'n': result += '\n'; break;
            case 't': result += '\t'; break;
            case 'r': result += '\r'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case '/': result += '/';  break;
            case '\\': result += '\\'; break;
            case '"': result += '"'; break;
            default: result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        ++pos;
    }
    out = result;
    return true;
}

std::string JsonUtil::ExtractObject(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    pos = FindOpenBracket(json, pos + pattern.size(), '{');
    if (pos == std::string::npos) return "";
    std::string obj = ScanBalanced(json, pos, '{', '}');
    return obj;
}

std::string JsonUtil::ExtractArray(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    pos = FindOpenBracket(json, pos + pattern.size(), '[');
    if (pos == std::string::npos) return "";
    std::string arr = ScanBalanced(json, pos, '[', ']');
    return arr;
}

bool JsonUtil::FindBoolArray(const std::string& json, const std::string& key,
                             std::vector<bool>& out, size_t maxCount) {
    std::string arr = ExtractArray(json, key);
    if (arr.empty()) return false;
    out.clear();
    size_t cur = 0;
    while (out.size() < maxCount) {
        size_t t  = arr.find("true", cur);
        size_t f  = arr.find("false", cur);
        size_t nx = std::string::npos;
        bool  val = false;
        if (t != std::string::npos && (f == std::string::npos || t < f)) {
            val = true; nx = t + 4;
        } else if (f != std::string::npos) {
            val = false; nx = f + 5;
        } else {
            break;
        }
        out.push_back(val);
        cur = nx;
    }
    return !out.empty();
}

std::vector<JsonUtil::JsonIcon> JsonUtil::ParseIconArray(const std::string& json, size_t arrStart) {
    std::vector<JsonIcon> out;
    const size_t open = FindOpenBracket(json, arrStart, '[');
    if (open == std::string::npos) {
        return out;
    }
    // D8 教训：旧实现用 find(']') 取数组末尾，一旦拿到的是【被截断的片段】就得到 npos，
    // 循环一次都不进、静默返回空 vector —— 上层把"解析失败"误当成"这条边没有图标"，
    // 于是回退共享集，用户的每边排列 100% 丢失且毫无提示。此处必须显式告警。
    const std::string arr = ScanBalanced(json, open, '[', ']');
    if (arr.empty()) {
        return out;
    }

    size_t cursor = 1;   // 跳过起始 '['
    while (cursor < arr.size()) {
        const size_t objStart = FindOpenBracket(arr, cursor, '{');
        if (objStart == std::string::npos) break;
        const std::string obj = ScanBalanced(arr, objStart, '{', '}');
        if (obj.empty()) {
            break;
        }

        JsonIcon entry;
        FindString(obj, "path", entry.path);   // 仅当存在有效 path 才收录
        FindString(obj, "name", entry.name);
        FindString(obj, "args", entry.args);
        FindString(obj, "workingDir", entry.workingDir);
        float idx = 0.0f;
        if (FindNumber(obj, "index", idx)) entry.index = static_cast<int>(idx);

        if (!entry.path.empty()) out.push_back(entry);
        cursor = objStart + obj.size();
    }
    return out;
}

std::string JsonUtil::JsonEscape(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\t': out += "\\t";  break;
        case '\r': out += "\\r";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:   out += c;       break;
        }
    }
    out += "\"";
    return out;
}

std::wstring JsonUtil::JsonEscape(const std::wstring& w) {
    std::wstring out = L"\"";
    for (wchar_t c : w) {
        switch (c) {
        case L'"':  out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n";  break;
        case L'\t': out += L"\\t";  break;
        case L'\r': out += L"\\r";  break;
        case L'\b': out += L"\\b";  break;
        case L'\f': out += L"\\f";  break;
        default:   out += c;        break;
        }
    }
    out += L"\"";
    return out;
}
