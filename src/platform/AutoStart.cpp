// src/platform/AutoStart.cpp
// 开机自启（HKCU Run 键）实现。关联：ADR_P3_design.md §3。
#include "AutoStart.h"
#include "../Common.h"
#include <shellapi.h>   // CommandLineToArgvW

namespace {

constexpr const wchar_t* kDefaultRunSubKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValueName = L"openDock";
constexpr const wchar_t* kAutoStartArg = L" --autostart";

// 测试注入点（默认为真实 Run 键）。非线程安全，仅测试单线程使用。
std::wstring g_runSubKey = kDefaultRunSubKey;

// 去掉首尾空白与包裹引号。
std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) --e;
    if (e - b >= 2 && s[b] == L'"' && s[e - 1] == L'"') { ++b; --e; }
    return s.substr(b, e - b);
}

// 尽可能把路径归一化到「最终 DOS 路径」：可打开文件时用 GetFinalPathNameByHandleW
// （抗 8.3 短名、junction/符号链接、大小写）；打不开（文件已被移走/删除）时退化为
// GetFullPathNameW（仅抗相对路径与 . ..），最终一律用 _wcsicmp 比较。
std::wstring NormalizePath(const std::wstring& raw) {
    const std::wstring p = Trim(raw);
    if (p.empty()) return p;

    HANDLE h = CreateFileW(p.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t buf[1024] = {};
        const DWORD n = GetFinalPathNameByHandleW(h, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])),
                                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(h);
        if (n > 0 && n < sizeof(buf) / sizeof(buf[0])) {
            std::wstring out(buf, n);
            // 去掉 \\?\ 前缀，便于与普通路径比较
            if (out.rfind(L"\\\\?\\", 0) == 0) out = out.substr(4);
            return out;
        }
    }
    wchar_t full[1024] = {};
    const DWORD n = GetFullPathNameW(p.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])),
                                     full, nullptr);
    if (n > 0 && n < sizeof(full) / sizeof(full[0])) return std::wstring(full, n);
    return p;
}

// 打开 Run 键。forWrite=true 时不存在会创建（测试注入的子键首次使用需要）。
LSTATUS OpenRunKey(bool forWrite, HKEY& outKey) {
    if (forWrite) {
        return RegCreateKeyExW(HKEY_CURRENT_USER, g_runSubKey.c_str(), 0, nullptr,
                               REG_OPTION_NON_VOLATILE,
                               KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &outKey, nullptr);
    }
    return RegOpenKeyExW(HKEY_CURRENT_USER, g_runSubKey.c_str(), 0, KEY_QUERY_VALUE, &outKey);
}

// 构造标准值数据： "<exe>" --autostart
std::wstring BuildRunValue(const std::wstring& exePath) {
    return L"\"" + Trim(exePath) + L"\"" + kAutoStartArg;
}

} // namespace

std::wstring AutoStart::CurrentExePath() {
    wchar_t buf[1024] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return (n > 0) ? std::wstring(buf, n) : std::wstring();
}

void AutoStart::SetRunSubKeyForTest(const std::wstring& subKey) {
    g_runSubKey = subKey.empty() ? kDefaultRunSubKey : subKey;
}

bool AutoStart::IsSameExecutable(const std::wstring& a, const std::wstring& b) {
    const std::wstring na = NormalizePath(a);
    const std::wstring nb = NormalizePath(b);
    if (na.empty() || nb.empty()) return false;
    return _wcsicmp(na.c_str(), nb.c_str()) == 0;
}

AutoStart::Query AutoStart::Read() {
    Query q;
    HKEY hKey = nullptr;
    LSTATUS st = OpenRunKey(/*forWrite=*/false, hKey);
    if (st != ERROR_SUCCESS) {
        // 子键不存在 ≠ 注册表故障：视为「未启用」（测试注入的子键首次读取即走此路）
        if (st == ERROR_FILE_NOT_FOUND) { q.status = Status::Disabled; return q; }
        q.status = Status::Error; q.lastError = (long)st;
        return q;
    }

    wchar_t buf[2048] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    st = RegQueryValueExW(hKey, kRunValueName, nullptr, &type,
                          reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(hKey);

    if (st == ERROR_FILE_NOT_FOUND) { q.status = Status::Disabled; return q; }
    if (st != ERROR_SUCCESS) {
        q.status = Status::Error; q.lastError = (long)st;
        return q;
    }
    // 用户手工改成 REG_EXPAND_SZ 也应识别为「已启用」，不能误判成未启用
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        q.status = Status::Error; q.lastError = (long)type;
        return q;
    }

    q.rawValue = buf;
    // 用 CommandLineToArgvW 回读 argv[0]，而不是手写引号剥离 —— 路径含空格/元字符时
    // 手写剥离容易出转义漏洞。
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(q.rawValue.c_str(), &argc);
    if (argv) {
        if (argc >= 1) q.exePath = argv[0];
        LocalFree(argv);
    }
    if (q.exePath.empty()) q.exePath = Trim(q.rawValue);

    q.status = IsSameExecutable(q.exePath, CurrentExePath())
             ? Status::EnabledCurrent : Status::EnabledStale;
    return q;
}

bool AutoStart::Enable(const std::wstring& exePath) {
    const std::wstring exe = exePath.empty() ? CurrentExePath() : exePath;
    if (exe.empty()) {
        return false;
    }
    const std::wstring value = BuildRunValue(exe);

    // 幂等：已是同值直接返回，避免无谓写注册表（启动项管理器会记录写入时间）
    const Query cur = Read();
    if (cur.status != Status::Error && cur.rawValue == value) return true;

    HKEY hKey = nullptr;
    LSTATUS st = OpenRunKey(/*forWrite=*/true, hKey);
    if (st != ERROR_SUCCESS) {
        return false;
    }
    st = RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(value.c_str()),
                        (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS) {
        return false;
    }

    // 可移动/网络位置上的 exe，开机时盘符可能尚未就绪 —— 只告警不阻止（无法在平台层弹气泡）
    if (exe.size() >= 2 && exe[1] == L':') {
        const std::wstring root = exe.substr(0, 2) + L"\\";
        const UINT dt = GetDriveTypeW(root.c_str());
        if (dt == DRIVE_REMOVABLE || dt == DRIVE_REMOTE) {
        }
    }
    return true;
}

bool AutoStart::Disable() {
    HKEY hKey = nullptr;
    LSTATUS st = OpenRunKey(/*forWrite=*/false, hKey);
    if (st == ERROR_FILE_NOT_FOUND) return true;    // 子键都没有，等价于已禁用
    if (st != ERROR_SUCCESS) {
        // 只读句柄不足以删除，改用可写句柄重开
    } else {
        RegCloseKey(hKey);
        hKey = nullptr;
    }
    st = RegCreateKeyExW(HKEY_CURRENT_USER, g_runSubKey.c_str(), 0, nullptr,
                         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (st != ERROR_SUCCESS) {
        return false;
    }
    st = RegDeleteValueW(hKey, kRunValueName);
    RegCloseKey(hKey);
    const bool ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
    return ok;
}

// ADR §3.3 真值表：配置为意图真值（intent），注册表为生效状态（state）。
// 启动期把 state 对齐到 intent；EnabledStale 是唯一例外 —— 它携带了配置不知道的信息。
//
//  config | 注册表状态       | 动作                  | effective
//  -------+------------------+-----------------------+-----------
//   true  | EnabledCurrent   | 无                    | true
//   true  | EnabledStale     | Enable()（自愈路径）  | true
//   true  | Disabled         | Enable()              | true
//   true  | Error            | 无（不假装成功）      | false
//   false | Disabled         | 无                    | false
//   false | EnabledCurrent   | Disable()             | false
//   false | EnabledStale     | 无（指向另一份程序，  | false
//         |                  |  无权替它做主删除）   |
//   false | Error            | 无                    | false
bool AutoStart::Reconcile(bool configWants, bool& outEffective) {
    const Query q = Read();
    bool wrote = false;

    if (configWants) {
        switch (q.status) {
        case Status::EnabledCurrent:
            outEffective = true;
            break;
        case Status::EnabledStale:
            wrote = Enable();
            outEffective = wrote;
            break;
        case Status::Disabled:
            wrote = Enable();
            outEffective = wrote;
            break;
        case Status::Error:
        default:
            // 注册表不可写（组策略/安全软件）：不写、不改配置，如实回报未生效
            outEffective = false;
            break;
        }
    } else {
        if (q.status == Status::EnabledCurrent) {
            wrote = Disable();
        }
        // EnabledStale：该值指向另一份 openDock，删掉会抢走别人的自启项 —— 绝不动
        outEffective = false;
    }
    return wrote;
}
