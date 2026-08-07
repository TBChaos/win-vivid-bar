cmake_minimum_required(VERSION 3.20)
project(openDock LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 静音 MSVC 安全 CRT 弃用警告（C4996: fopen 等）。
# 项目混合使用 _s 安全函数与原生 fopen，统一用此宏关闭唠叨，不改变行为。
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# ═══ 调试模式开关（沙盒文本化验证用）═══
# ON  → 控制台子系统（stdout 可见）+ DOCK_DEBUG_MODE 宏
# OFF → Windows 子系统（无控制台，发布形态）
option(DOCK_DEBUG_MODE "Enable debug output and console subsystem" ON)

# ═══ 源文件收集 ═══
file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "src/*.h")

# ═══ 主目标 ═══
add_executable(openDock ${SOURCES} ${HEADERS})
target_include_directories(openDock PRIVATE src)

# ═══ 链接系统库 ═══
target_link_libraries(openDock PRIVATE
    d2d1 dwrite dcomp d3d11 dxgi windowscodecs
    dwmapi shell32 ole32 user32 gdi32 advapi32 uuid
)

# ═══ 编译选项 ═══
target_compile_options(openDock PRIVATE /utf-8 /W4 /EHsc)

if(DOCK_DEBUG_MODE)
    target_compile_definitions(openDock PRIVATE DOCK_DEBUG_MODE=1)
    set_target_properties(openDock PROPERTIES WIN32_EXECUTABLE FALSE)
    message(STATUS "openDock: DEBUG MODE (console subsystem)")
else()
    set_target_properties(openDock PROPERTIES WIN32_EXECUTABLE TRUE)
    target_compile_options(openDock PRIVATE /O2 /GL)
    target_link_options(openDock PRIVATE /LTCG)
    message(STATUS "openDock: RELEASE MODE (windows subsystem)")
endif()

# ═══ 构建戳（消解「陈旧二进制」歧义）═══
# 编译期注入 OPENDOCK_BUILD_TIME（构建时间）与 OPENDOCK_BUILD_HASH（git HEAD 短哈希，取不到为 unknown）。
# DiagLog 会在每条诊断日志的首行打印 [BUILD] date=... hash=...，便于真机复测时核对二进制版本。
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
