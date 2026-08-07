// src/debug/DebugExporter.h
#pragma once
#include "../Common.h"
#include <fstream>
#include <map>

class DebugExporter {
public:
    static DebugExporter& Instance() {
        static DebugExporter inst;
        return inst;
    }

    // 写入简单 KV 状态文件
    void WriteStatus(const std::string& filename,
                     const std::map<std::string, std::string>& kv) {
        std::string path = "debug_output/" + filename + ".json";
        std::ofstream out(path);
        if (!out.is_open()) {
            return;
        }
        out << "{\n";
        bool first = true;
        for (auto& [k, v] : kv) {
            if (!first) out << ",\n";
            out << "  \"" << k << "\": \"" << v << "\"";
            first = false;
        }
        out << "\n}\n";
        out.close();
    }

    // 写入弹簧系统快照
    void DumpSprings(const std::string& filename,
                     const std::vector<std::map<std::string, float>>& springs,
                     int frame, bool allSettled) {
        std::string path = "debug_output/" + filename + ".json";
        std::ofstream out(path);
        if (!out.is_open()) return;

        out << "{\n";
        out << "  \"frame\": " << frame << ",\n";
        out << "  \"all_settled\": " << (allSettled ? "true" : "false") << ",\n";
        out << "  \"springs\": [\n";
        for (size_t i = 0; i < springs.size(); i++) {
            out << "    {";
            bool f = true;
            for (auto& [k, v] : springs[i]) {
                if (!f) out << ",";
                out << "\"" << k << "\":" << v;
                f = false;
            }
            out << "}";
            if (i < springs.size() - 1) out << ",";
            out << "\n";
        }
        out << "  ]\n}\n";
        out.close();
    }

    // 写入布局快照
    void DumpLayouts(const std::string& filename,
                     const std::vector<std::map<std::string, float>>& layouts) {
        std::string path = "debug_output/" + filename + ".json";
        std::ofstream out(path);
        if (!out.is_open()) return;

        out << "{\n  \"layouts\": [\n";
        for (size_t i = 0; i < layouts.size(); i++) {
            out << "    {";
            bool f = true;
            for (auto& [k, v] : layouts[i]) {
                if (!f) out << ",";
                out << "\"" << k << "\":" << v;
                f = false;
            }
            out << "}";
            if (i < layouts.size() - 1) out << ",";
            out << "\n";
        }
        out << "  ]\n}\n";
        out.close();
    }

private:
    DebugExporter() {
        CreateDirectoryA("debug_output", nullptr);
    }
};
