// src/utils/PathUtil.cpp
// 统一路径工具实现（合并自 DockEngine / ConfigManager / DockManager 的三份复制）。
// 关联：优化架构设计 §6.1（P1-3）。
#include "PathUtil.h"
#include <windows.h>

std::wstring PathUtil::Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
    return w;
}

std::string PathUtil::WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

bool PathUtil::IsAbsolutePath(const std::wstring& p) {
    return p.size() >= 2 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'));
}

std::wstring PathUtil::GetExeDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p = p.substr(0, slash + 1);
    return p;
}

std::string PathUtil::ResolveConfigPath(const std::string& input, const std::wstring& exeDir) {
    const wchar_t* relCands[] = { L"res/config.json", L"config.json" };

    // 1) 给定路径直接可用（绝对或相对 CWD）
    if (GetFileAttributesW(Utf8ToWide(input).c_str()) != INVALID_FILE_ATTRIBUTES)
        return input;

    // 2) exe 同目录候选
    for (auto c : relCands) {
        std::wstring p = exeDir + c;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            return WideToUtf8(p);
    }

    // 3) 当前工作目录候选（开发 / 沙盒从项目根运行）
    for (auto c : relCands) {
        if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES)
            return WideToUtf8(std::wstring(c));
    }

    // 4) 兜底：exe 同目录 res/config.json（触发默认配置回退）
    return WideToUtf8(exeDir + L"res/config.json");
}

std::wstring PathUtil::DeriveDisplayName(const std::wstring& path) {
    if (path.empty()) return L"";

    // 1) 剥尾部分隔符，避免 C:\Users\Foo\Documents\ 这类尾分隔致末级为空
    std::wstring p = path;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();

    // 取末级文件名
    std::wstring name = p;
    size_t bs = name.find_last_of(L"\\/");
    if (bs != std::wstring::npos) name = name.substr(bs + 1);

    // 2) 驱动器根（"D:" / "D:\" 剥尾后仍是 "D:"）-> 回退盘符本身，绝不空
    if (name.empty()) {
        if (p.size() >= 2 && p[1] == L':') return p.substr(0, 2);
        return p.empty() ? path : p;
    }

    // 3) 仅末级是“文件”时才去扩展名：dot>0 排除点开头（.config），
    //    用 GetFileAttributesW 判是否为目录；不存在时按文件处理（扩展名照去）。
    //    —— 文件夹名里的点（backup.old）不被当作扩展名分隔符。
    bool isDir = false;
    DWORD attr = GetFileAttributesW(p.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

    size_t dot = name.find_last_of(L".");
    if (!isDir && dot > 0)
        name = name.substr(0, dot);

    // 4) 兜底不变量：派生完若仍空，回退末级原文，再不行回退完整 path，永不空白
    if (name.empty()) {
        name = p;
        size_t bs2 = name.find_last_of(L"\\/");
        if (bs2 != std::wstring::npos) name = name.substr(bs2 + 1);
    }
    if (name.empty()) name = path;
    return name;
}
