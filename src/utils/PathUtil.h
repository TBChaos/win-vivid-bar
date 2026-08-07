// src/utils/PathUtil.h
// 统一路径工具（合并自 DockEngine / ConfigManager / DockManager 三处复制实现）
//
// 设计要点：
//   * Utf8ToWide / WideToUtf8 / IsAbsolutePath 为纯字符串转换（无文件系统访问），可单测。
//   * GetExeDir / ResolveConfigPath 使用 Win32 解析真实路径。
// 关联：优化架构设计 §6.1（P1-3）。
#pragma once
#include <string>
#include <vector>

struct PathUtil {
    // —— 纯字符串（零文件访问，可单测）——
    static std::wstring Utf8ToWide(const std::string& s);
    static std::string  WideToUtf8(const std::wstring& w);
    static bool         IsAbsolutePath(const std::wstring& p);

    // —— 依赖 Win32（运行时路径解析）——
    static std::wstring GetExeDir();   // exe 所在目录（带结尾反斜杠）
    // 解析配置文件路径：给定路径 → exe 同目录候选 → CWD 候选 → 兜底 exe 同目录 res/config.json
    static std::string  ResolveConfigPath(const std::string& input, const std::wstring& exeDir);

    // 从路径派生图标显示名（边界安全的统一实现，供 AddIcon / AddIconFromDrop 共用）：
    //   1) 先剥尾部分隔符（C:\Users\Foo\Documents\ -> Documents），避免尾分隔致空名；
    //   2) 剥完若为空（驱动器根 D: / D:\）回退盘符本身，绝不空；
    //   3) 仅当末级是“文件”时才去扩展名：dot>0 排除点开头（.config），
    //      且用 GetFileAttributesW 判目录；不存在(INVALID_FILE_ATTRIBUTES)按文件处理
    //      —— 文件夹名里的点（backup.old）不当扩展名截断；
    //   4) 兜底不变量：派生完若仍空，回退末级原文，再不行回退完整 path，永不空白。
    // 依赖 Win32（GetFileAttributesW 区分目录/文件）。
    static std::wstring DeriveDisplayName(const std::wstring& path);
};
