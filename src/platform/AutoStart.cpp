// src/platform/AutoStart.cpp
// 开机自启实现。关联：ADR_P3_design.md §3。
//
// 机制（2026-08 重构）：
//   主路径 = Windows 计划任务（logon 触发器）。Task Scheduler 服务起得很早，
//   计划任务在「用户登录瞬间」由该服务直接拉起，不受 Explorer 对 Run 键启动项
//   的错峰延迟（startup app staggering，约 1 分钟）影响 —— 这是根治开机慢的关键。
//   注意：计划任务的「创建」在标准用户 / 受限策略下会「拒绝访问」，故不可作为
//   唯一路径。
//   兜底路径 = HKCU\Run 键（无需管理员，标准用户可写）+ 写入
//   HKCU\...\Explorer\Serialize\StartupDelayInMSec=0 关闭 Windows 对启动项的错峰延迟，
//   把 Run 键路径也压回秒级。
//
// 历史：原实现只用 Run 键，实测登录后约 1 分钟才被拉起；改为「计划任务优先 +
// 关启动延迟的 Run 键兜底」后，标准用户也能秒级自启。
#include "AutoStart.h"
#include "../Common.h"
#include <shellapi.h>   // CommandLineToArgvW
#include <vector>

namespace {

constexpr const wchar_t* kDefaultRunSubKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValueName = L"openDock";
constexpr const wchar_t* kAutoStartArg = L" --autostart";

// 计划任务名称（当前用户会话内唯一，由本程序独占）。
constexpr const wchar_t* kTaskName = L"openDock";

// 测试注入点（默认为真实 Run 键）。非线程安全，仅测试单线程使用。
std::wstring g_runSubKey = kDefaultRunSubKey;

// ───────────────────────── Run 键辅助（兜底路径） ─────────────────────────

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

// ───────────────────────── 计划任务辅助（主路径） ─────────────────────────
// 通过 schtasks.exe 落地，避免直接调 Task Scheduler COM（STA/MTA 初始化耦合主线程）。

// 取得 %TEMP% 下的一个稳定文件名（路径可能含空格，调用方需自行加引号）。
std::wstring TempFilePath(const std::wstring& name) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + name;
}

// 写 UTF-16LE（带 BOM）文件。schtasks /XML 要求 UTF-16 编码。
bool WriteUtf16File(const std::wstring& path, const std::wstring& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const WORD bom = 0xFEFF;
    DWORD written = 0;
    BOOL ok = WriteFile(h, &bom, sizeof(bom), &written, nullptr);
    if (ok) WriteFile(h, content.c_str(),
                      (DWORD)(content.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    return ok != 0;
}

// 读取整文件，按 UTF-16（带或不带 BOM）解释为宽字符串。
std::wstring ReadWholeFileUtf16(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD size = GetFileSize(h, nullptr);
    std::wstring out;
    if (size > 0) {
        std::vector<char> buf(size);
        DWORD rd = 0;
        if (ReadFile(h, buf.data(), size, &rd, nullptr) && rd > 0) {
            const BYTE* p = reinterpret_cast<const BYTE*>(buf.data());
            if (rd >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
                out.assign(reinterpret_cast<const wchar_t*>(p + 2),
                           (rd - 2) / sizeof(wchar_t));
            } else {
                out.assign(reinterpret_cast<const wchar_t*>(p), rd / sizeof(wchar_t));
            }
        }
    }
    CloseHandle(h);
    return out;
}

// 运行 schtasks.exe，stdout/stderr 重定向到 outFile，返回退出码。
bool RunSchtasks(const std::wstring& args, const std::wstring& outFile, DWORD& exitCode) {
    wchar_t sysdir[MAX_PATH] = {};
    GetSystemDirectoryW(sysdir, MAX_PATH);
    const std::wstring exe = std::wstring(sysdir) + L"\\schtasks.exe";

    std::wstring cmd = L"schtasks " + args;   // 由 schtasks.exe 自行解析 argv

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hOut = CreateFileW(outFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) return false;

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hOut;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOut);
    if (!ok) return false;

    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// XML 文本转义（仅 exe 路径可能含 & < > "）。
std::wstring XmlEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'&') o += L"&amp;";
        else if (c == L'<') o += L"&lt;";
        else if (c == L'>') o += L"&gt;";
        else if (c == L'"') o += L"&quot;";
        else o += c;
    }
    return o;
}

// 构造 logon 触发器任务定义（UTF-16）。
std::wstring BuildTaskXml(const std::wstring& exe) {
    return
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Description>openDock auto-start (logon)</Description>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Triggers>\r\n"
        L"    <LogonTrigger><Enabled>true</Enabled></LogonTrigger>\r\n"
        L"  </Triggers>\r\n"
        L"  <Principals>\r\n"
        L"    <Principal id=\"Author\">\r\n"
        L"      <LogonType>InteractiveToken</LogonType>\r\n"
        L"      <RunLevel>LeastPrivilege</RunLevel>\r\n"
        L"    </Principal>\r\n"
        L"  </Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"    <Priority>7</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\">\r\n"
        L"    <Exec>\r\n"
        L"      <Command>" + XmlEscape(exe) + L"</Command>\r\n"
        L"      <Arguments>--autostart</Arguments>\r\n"
        L"    </Exec>\r\n"
        L"  </Actions>\r\n"
        L"</Task>\r\n";
}

// 计划任务是否存在（退出码 0 = 存在）。
bool ScheduledTaskExists() {
    const std::wstring out = TempFilePath(L"openDock_schtask_query.log");
    DWORD ec = 0;
    if (!RunSchtasks(L"/Query /TN \"" + std::wstring(kTaskName) + L"\"", out, ec))
        return false;
    return ec == 0;
}

// 读取计划任务的 Command / Arguments（从 /XML ONE 输出解析）。
bool GetScheduledTaskCommand(std::wstring& outCmd, std::wstring& outArgs) {
    const std::wstring out = TempFilePath(L"openDock_schtask_query.xml");
    DWORD ec = 0;
    if (!RunSchtasks(L"/Query /TN \"" + std::wstring(kTaskName) + L"\" /XML ONE", out, ec))
        return false;
    if (ec != 0) return false;

    const std::wstring xml = ReadWholeFileUtf16(out);
    auto Extract = [&](const wchar_t* tag, std::wstring& dst) -> bool {
        const std::wstring open = std::wstring(L"<") + tag + L">";
        const std::wstring close = std::wstring(L"</") + tag + L">";
        const size_t a = xml.find(open);
        if (a == std::wstring::npos) return false;
        const size_t b = xml.find(close, a + open.size());
        if (b == std::wstring::npos) return false;
        dst = xml.substr(a + open.size(), b - (a + open.size()));
        return true;
    };
    return Extract(L"Command", outCmd) && Extract(L"Arguments", outArgs);
}

// 创建（或覆盖）计划任务。成功返回 true。
bool CreateScheduledTask(const std::wstring& exe) {
    const std::wstring xmlPath = TempFilePath(L"openDock_schtask.xml");
    if (!WriteUtf16File(xmlPath, BuildTaskXml(exe))) return false;

    const std::wstring out = TempFilePath(L"openDock_schtask_create.log");
    DWORD ec = 0;
    const std::wstring args = L"/Create /F /TN \"" + std::wstring(kTaskName) +
                              L"\" /XML \"" + xmlPath + L"\"";
    if (!RunSchtasks(args, out, ec)) return false;
    return ec == 0;
}

// 删除计划任务（best-effort：不存在也视为成功）。
bool DeleteScheduledTask() {
    const std::wstring out = TempFilePath(L"openDock_schtask_delete.log");
    DWORD ec = 0;
    if (!RunSchtasks(L"/Delete /TN \"" + std::wstring(kTaskName) + L"\" /F", out, ec))
        return false;
    return true;
}

// ───────────────────────── Run 键读写（兜底，供主路径调用） ─────────────────────────

bool EnableRunKey(const std::wstring& exe) {
    const std::wstring value = BuildRunValue(exe);
    HKEY hKey = nullptr;
    LSTATUS st = OpenRunKey(/*forWrite=*/true, hKey);
    if (st != ERROR_SUCCESS) return false;
    st = RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(value.c_str()),
                        (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
}

bool DisableRunKey() {
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
    if (st != ERROR_SUCCESS) return false;
    st = RegDeleteValueW(hKey, kRunValueName);
    RegCloseKey(hKey);
    return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
}

// 关闭 Windows 对启动项（Run 键 / 启动文件夹）的错峰延迟（startup app staggering，约 1 分钟）。
// 写入 HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Serialize\StartupDelayInMSec=0。
// 该键位于 HKCU，标准用户即可写，无需管理员；下次登录生效。
// 这是「标准用户无法创建计划任务」场景下消除自启延迟的可靠手段。
bool DisableStartupDelay() {
    HKEY hKey = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize",
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (st != ERROR_SUCCESS) return false;
    const DWORD zero = 0;
    st = RegSetValueExW(hKey, L"StartupDelayInMSec", 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
    RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
}

// 仅查询 Run 键状态（不含计划任务）。
AutoStart::Query ReadRunKey() {
    AutoStart::Query q;
    HKEY hKey = nullptr;
    LSTATUS st = OpenRunKey(/*forWrite=*/false, hKey);
    if (st != ERROR_SUCCESS) {
        if (st == ERROR_FILE_NOT_FOUND) { q.status = AutoStart::Status::Disabled; return q; }
        q.status = AutoStart::Status::Error; q.lastError = (long)st;
        return q;
    }

    wchar_t buf[2048] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    st = RegQueryValueExW(hKey, kRunValueName, nullptr, &type,
                          reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(hKey);

    if (st == ERROR_FILE_NOT_FOUND) { q.status = AutoStart::Status::Disabled; return q; }
    if (st != ERROR_SUCCESS) {
        q.status = AutoStart::Status::Error; q.lastError = (long)st;
        return q;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        q.status = AutoStart::Status::Error; q.lastError = (long)type;
        return q;
    }

    q.rawValue = buf;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(q.rawValue.c_str(), &argc);
    if (argv) {
        if (argc >= 1) q.exePath = argv[0];
        LocalFree(argv);
    }
    if (q.exePath.empty()) q.exePath = Trim(q.rawValue);

    q.status = AutoStart::IsSameExecutable(q.exePath, AutoStart::CurrentExePath())
             ? AutoStart::Status::EnabledCurrent : AutoStart::Status::EnabledStale;
    return q;
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

// 综合查询：优先看计划任务，缺位时回退 Run 键。
AutoStart::Query AutoStart::Read() {
    if (ScheduledTaskExists()) {
        std::wstring cmd, args;
        if (GetScheduledTaskCommand(cmd, args)) {
            Query q;
            q.exePath = cmd;
            q.rawValue = L"\"" + cmd + L"\" " + args;   // 与 Run 键值格式对齐，便于日志/UI
            q.status = IsSameExecutable(cmd, CurrentExePath())
                     ? Status::EnabledCurrent : Status::EnabledStale;
            return q;
        }
        // 任务存在但解析失败：保守按 Run 键兜底
    }
    return ReadRunKey();
}

// 主路径 = 计划任务；成功则清掉 Run 键避免重复拉起。
// 失败（标准用户/受限策略会「拒绝访问」）回退 Run 键，并关闭 Windows 启动项错峰延迟，
// 把 Run 键路径也压回秒级。
bool AutoStart::Enable(const std::wstring& exePath) {
    const std::wstring exe = exePath.empty() ? CurrentExePath() : exePath;
    if (exe.empty()) return false;

    if (CreateScheduledTask(exe)) {
        DisableRunKey();   // 主路径已接管，移除兜底键以免双实例
        return true;
    }
    const bool ok = EnableRunKey(exe);
    DisableStartupDelay();   // 关掉 OS 对 Run 键的 ~1 分钟错峰延迟（HKCU，无需管理员）
    return ok;
}

// 同时清理计划任务与 Run 键。
bool AutoStart::Disable() {
    const bool okTask = DeleteScheduledTask();
    const bool okRun = DisableRunKey();
    return okTask && okRun;
}

// 启动期对齐「配置意图」与「生效状态」（基于综合 Read）。
// 计划任务为主：只要自启意图为真且无任务，就建任务（顺便迁移旧 Run 键）；
// 任务已存在且指向他人 exe 则重指；意图为假则两者皆清。
bool AutoStart::Reconcile(bool configWants, bool& outEffective) {
    const Query q = Read();
    bool wrote = false;

    if (configWants) {
        if (!ScheduledTaskExists()) {
            // 首次建任务，或从旧版 Run 键迁移
            wrote = Enable();
            outEffective = wrote;
        } else if (q.status == Status::EnabledStale) {
            wrote = Enable();   // 重指到当前 exe
            outEffective = wrote;
        } else {
            // 任务已存在且指向本程序：确保没有遗留 Run 键造成双拉起
            DisableRunKey();
            outEffective = true;
        }
    } else {
        if (q.status == Status::EnabledCurrent || q.status == Status::EnabledStale) {
            wrote = Disable();
        }
        outEffective = false;
    }
    return wrote;
}
