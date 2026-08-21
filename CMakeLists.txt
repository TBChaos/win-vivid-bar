cmake_minimum_required(VERSION 3.20)
project(openDock LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 静音 MSVC 安全 CRT 弃用警告（C4996: fopen 等）。
# 项目混合使用 _s 安全函数与原生 fopen，统一用此宏关闭唠叨，不改变行为。
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# ═══ 源文件收集 ═══
file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "src/*.h")

# ═══ 主目标 ═══
# 应用图标资源（MAINICON）与托盘图标保持一致，编入 exe 使文件资源管理器 /
# 任务栏 / 任务管理器显示同一图标。改图标后用 tools/gen_app_icon.py 重生成 res/icons/tray_icon.ico。
# 注意：CMake 默认对 .rc 走 cmcldeps 依赖扫描，在部分环境下 cmcldeps.exe 无法启动
# （CreateProcess: %1 is not a valid Win32 application）。这里改为直接用 rc.exe 预编译为 .res，
# 再作为链接输入编入 exe，规避 cmcldeps。
set(APP_RC "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
set(APP_RES "${CMAKE_CURRENT_BINARY_DIR}/app.res")
add_custom_command(
    OUTPUT "${APP_RES}"
    COMMAND "${CMAKE_RC_COMPILER}" /nologo /fo "${APP_RES}" "${APP_RC}"
    DEPENDS "${APP_RC}"
    COMMENT "Compiling app.rc -> app.res (app icon)"
    VERBATIM)
add_executable(openDock ${SOURCES} ${HEADERS} ${APP_RES})
# 关键修复（2026-08-21）：add_executable 里的 app.res 在 Ninja 下仅被当作
# 「顺序依赖」(order-only)，不会落到 link.exe 命令行，导致 exe 不嵌入任何图标，
# 文件/任务栏图标与托盘图标全部丢失（二者都回退为系统默认图标）。
# 用 target_link_options 把已编译的 .res 显式塞进链接命令行，确保 RT_GROUP_ICON 真正编入 exe。
target_link_options(openDock PRIVATE "${APP_RES}")
target_include_directories(openDock PRIVATE src)

# ═══ 链接系统库 ═══
target_link_libraries(openDock PRIVATE
    d2d1 dwrite dcomp d3d11 dxgi windowscodecs
    dwmapi shell32 ole32 user32 gdi32 advapi32 uuid
)

# ═══ 编译选项 ═══
target_compile_options(openDock PRIVATE /utf-8 /W4 /EHsc)

# 仅保留发布形态：Windows 子系统（无控制台）+ 优化。
# 诊断日志与无头仿真驱动已整体移除，不再提供 debug 构建链路。
set_target_properties(openDock PROPERTIES WIN32_EXECUTABLE TRUE)
target_compile_options(openDock PRIVATE /O2 /GL)
target_link_options(openDock PRIVATE /LTCG)
message(STATUS "openDock: RELEASE MODE (windows subsystem)")

# ═══ 构建戳（消解「陈旧二进制」歧义）═══
# 编译期注入 OPENDOCK_BUILD_TIME（构建时间）与 OPENDOCK_BUILD_HASH（git HEAD 短哈希，取不到为 unknown）。
# 构建戳便于真机复测时核对二进制版本（诊断日志已移除，不再依赖日志首行打印）。
string(TIMESTAMP OPENDOCK_BUILD_TIME "%Y-%m-%d %H:%M:%S")
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE OPENDOCK_BUILD_HASH_RAW
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT OPENDOCK_BUILD_HASH_RAW)
    set(OPENDOCK_BUILD_HASH_RAW "unknown")
endif()
target_compile_definitions(openDock PRIVATE
    "OPENDOCK_BUILD_TIME=\"${OPENDOCK_BUILD_TIME}\""
    "OPENDOCK_BUILD_HASH=\"${OPENDOCK_BUILD_HASH_RAW}\"")

# 注：单元测试目标（test_*）与测试夹具复制已移除——仓库不再随附 tests/，回归纯应用构建。

# ═══ 复制默认配置到构建目录 ═══
# 同时复制到 <build>/config.json 与 <build>/res/config.json，
# 以兼容双击 exe（CWD=build）时「exe 同目录」的两种候选解析路径
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/config.json")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/res/config.json
                   ${CMAKE_CURRENT_BINARY_DIR}/config.json COPYONLY)
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/res/config.json"
         DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/res")
endif()

# ═══ 复制图标资源到构建目录（P2-3：按 mtime 增量刷新）═══
# 双击 exe 时 CWD = 构建目录，config.json 内图标路径（res/icons/...）需在此可解析
# （DockEngine::InitializeFromFile 会将相对图标路径解析为「exe 目录」绝对路径）。
# 原 file(COPY) 在 configure 阶段执行、不随源图标 mtime 变化刷新，改图标需清缓存重建；
# 改为构建期 POST_BUILD 步骤：每次增量构建都用 copy_directory 同步最新图标，
# 修改 res/icons 后无需手动清缓存即可生效。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/icons")
    add_custom_target(copy_dock_icons ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/res/icons"
                "${CMAKE_CURRENT_BINARY_DIR}/res/icons"
        COMMENT "Syncing res/icons (P2-3: mtime refresh on incremental build)"
        VERBATIM)
endif()
