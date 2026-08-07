// src/utils/DiagLog.cpp
// 统一诊断日志实现（合并 DockEngine / IconProvider / RenderManager 三处诊断日志函数）。
// 关联：优化架构设计 §6.2（P1-4）。
#include "DiagLog.h"
#include <cstdio>
#include <windows.h>
#include <set>
#include <string>

// 构建戳（由 CMake 注入；未注入时回退 unknown，保证独立/测试编译亦可通过）
#ifndef OPENDOCK_BUILD_TIME
#define OPENDOCK_BUILD_TIME "unknown"
#endif
#ifndef OPENDOCK_BUILD_HASH
#define OPENDOCK_BUILD_HASH "unknown"
#endif

namespace {
    // 每个 tag 在一次进程内仅写一次构建戳，置于该日志本运行的首行
    std::set<std::string>& StampedTags() {
        static std::set<std::string> s;
        return s;
    }
}

void DiagLog(const char* tag, const char* fmt, ...) {
    // 拼接文件名：debug_output/openDock_<tag>.log
    const char* t = tag ? tag : "app";
    char fileName[MAX_PATH] = { 0 };
    _snprintf_s(fileName, _TRUNCATE, "debug_output/openDock_%s.log", t);

    va_list ap; va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    CreateDirectoryA("debug_output", nullptr);
    // 本 tag 日志在本次进程内首次写入时，以 "w" 截断旧日志，使构建戳成为文件首行；
    // 之后以 "a" 追加，保留本运行内的后续日志。这样即便日志文件已存在（累积多段运行），
    // [BUILD] 始终位于第 1 行，真机复测时一眼即可确认跑的是哪次构建。
    bool firstForTag = StampedTags().count(std::string(t)) == 0;
    FILE* fp = fopen(fileName, firstForTag ? "w" : "a");
    if (fp) {
        if (firstForTag) {
            StampedTags().insert(std::string(t));
            fprintf(fp, "[BUILD] date=%s hash=%s\n", OPENDOCK_BUILD_TIME, OPENDOCK_BUILD_HASH);
        }
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(fp, "[%02d:%02d:%02d.%03d] [%s] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                t, buf);
        fclose(fp);
    }
}
