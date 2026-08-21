// src/platform/AutoStart.h
// 开机自启平台设施。从 ConfigManager 搬出：注册表/计划任务访问属于平台能力，
// 不属于配置序列化的职责。关联：ADR_P3_design.md §3（ADR-3 开机自启）。
//
// 机制（2026-08 重构）：
//   主路径 = 计划任务（logon 触发器，名称 "openDock"）。Task Scheduler 服务起得很早，
//   在「用户登录瞬间」直接拉起本程序，不受 Explorer 对 Run 键启动项的错峰延迟
//   （startup app staggering，实测约 1 分钟）影响 —— 根治开机自启慢。
//   注意：计划任务的「创建」在标准用户 / 受限策略下会被「拒绝访问」，故不可作唯一路径。
//   兜底路径 = HKCU\Run 键 + 写入 HKCU\...\Explorer\Serialize\StartupDelayInMSec=0
//   关闭 Windows 对启动项的错峰延迟，把 Run 键路径也压回秒级（标准用户即可，无需管理员）。
//
// Run 键规格（兜底，固定，勿随版本变化）：
//   根键   : HKEY_CURRENT_USER
//   子键   : Software\Microsoft\Windows\CurrentVersion\Run（测试可注入，见 SetRunSubKeyForTest）
//   值名   : openDock
//   值类型 : REG_SZ（读取时同时接受 REG_EXPAND_SZ）
//   值数据 : "<exe 绝对路径>" --autostart
//            - 恒加引号（路径可能含空格或 & ^ ( ) 等元字符，恒加是唯一无歧义写法）
//            - --autostart 供进程自识别"本次是开机拉起"（延迟建窗，避开桌面初始化）
#pragma once
#include <string>

namespace AutoStart {

enum class Status {
    Disabled,        // 注册表无该值
    EnabledCurrent,  // 有值，且解析出的 exe 路径 == 当前进程 exe（归一化后）
    EnabledStale,    // 有值，但指向别处（exe 被移动/改名，或另一份 openDock 抢注）
    Error            // 注册表访问失败（权限/策略/损坏）
};

struct Query {
    Status       status    = Status::Error;
    std::wstring rawValue;        // 注册表原始值（含引号与参数），供日志与 UI 展示
    std::wstring exePath;         // 从 rawValue 解析出的 exe 绝对路径（已去引号、去参数）
    long         lastError = 0;   // 失败时的 LSTATUS
};

// 写入：值 = "\"<exePath>\" --autostart"。exePath 为空则取当前进程模块路径。
// 幂等：已是同值时直接返回 true，不重复写。
bool  Enable(const std::wstring& exePath = L"");

// 删除：值不存在视为成功（ERROR_FILE_NOT_FOUND -> true）。
bool  Disable();

// 查询：不修改任何状态。
Query Read();

// 启动期调用【一次】。按 ADR §3.3 真值表把「注册表状态」对齐到「配置意图」，
// 并把最终生效值写入 outEffective（调用方据此更新 AppConfig.autoStart 并置脏）。
// 返回值：注册表是否发生了实际写入（用于决定要不要落盘配置 / 记日志）。
bool  Reconcile(bool configWants, bool& outEffective);

// 路径归一化比较：抗大小写、8.3 短名、符号链接、尾随空格与引号。
bool  IsSameExecutable(const std::wstring& a, const std::wstring& b);

// 当前进程 exe 的绝对路径。
std::wstring CurrentExePath();

// —— 测试注入 ——
// 覆盖 Run 子键路径（例如 L"Software\\openDockTest"），避免单测污染用户真实启动项。
// 传空串恢复默认。仅供测试使用。
void SetRunSubKeyForTest(const std::wstring& subKey);

} // namespace AutoStart
