// src/utils/DiagLog.h
// 统一诊断日志入口（合并自 DockEngine::AppendDiagLog / IconProvider::DiagLogIP /
// RenderManager::DiagLogRM 三处近同实现）。
//
// 设计要点：
//   * 单一入口 void DiagLog(tag, fmt, ...)；tag 决定文件名 debug_output/openDock_<tag>.log，
//     模块前缀体现在消息体（[tag] ...）。
//   * 与 DOCK_LOG 宏的区别：DOCK_LOG 受 DOCK_DEBUG_MODE 编译期剥离（Release 无每帧日志）；
//     DiagLog 始终写文件（用于真实 GUI 排查的持久诊断，不受编译期宏影响）。
//   * 调用方约定：引擎层 tag="engine"，图标层 tag="icon"，渲染层 tag="render"。
// 关联：优化架构设计 §6.2（P1-4）。
#pragma once
#include <cstdarg>

void DiagLog(const char* tag, const char* fmt, ...);
