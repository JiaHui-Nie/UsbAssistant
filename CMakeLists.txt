cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# 版本号：默认从 git tag 推导。
# tag 正好指向 HEAD 时是正式版本；分支、脏工作区、无 tag 都标记为开发版本，
# 不会冒充正式版本。GitHub Actions 同样只依赖这份推导逻辑。
# 也可通过 -DUSBASSISTANT_VERSION_OVERRIDE=1.2.3 直接指定编译版本。
# ---------------------------------------------------------------------------
set(USBASSISTANT_VERSION_OVERRIDE "" CACHE STRING
    "手动指定编译版本（如 1.2.3）；留空则按 git tag / git describe 推导")

set(USBASSISTANT_FALLBACK_VERSION "1.2.1")
set(USBASSISTANT_VERSION "${USBASSISTANT_FALLBACK_VERSION}")
set(USBASSISTANT_VERSION_DISPLAY "dev")
set(USBASSISTANT_VERSION_IS_RELEASE OFF)
set(USBASSISTANT_VERSION_SOURCE "git")

if(USBASSISTANT_VERSION_OVERRIDE)
    string(STRIP "${USBASSISTANT_VERSION_OVERRIDE}"
           USBASSISTANT_VERSION_OVERRIDE_VALUE)
    string(REGEX REPLACE "^[vV]" "" USBASSISTANT_VERSION_OVERRIDE_VALUE
           "${USBASSISTANT_VERSION_OVERRIDE_VALUE}")
    if(NOT USBASSISTANT_VERSION_OVERRIDE_VALUE MATCHES
       "^[0-9]+(\\.[0-9]+)*$")
        message(FATAL_ERROR
            "USBASSISTANT_VERSION_OVERRIDE 必须是 1.2.3 这样的数字版本："
            "${USBASSISTANT_VERSION_OVERRIDE}")
    endif()
    set(USBASSISTANT_VERSION "${USBASSISTANT_VERSION_OVERRIDE_VALUE}")
    set(USBASSISTANT_VERSION_DISPLAY "v${USBASSISTANT_VERSION_OVERRIDE_VALUE}")
    set(USBASSISTANT_VERSION_IS_RELEASE ON)
    set(USBASSISTANT_VERSION_SOURCE "override")
else()
    find_program(USBASSISTANT_GIT git)
    if(USBASSISTANT_GIT)
        execute_process(
            COMMAND "${USBASSISTANT_GIT}" describe --tags --always --dirty
                    --match "v*"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE USBASSISTANT_GIT_DESCRIBE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()

    if(USBASSISTANT_GIT_DESCRIBE MATCHES "^v([0-9]+(\\.[0-9]+)+)$")
        set(USBASSISTANT_VERSION "${CMAKE_MATCH_1}")
        set(USBASSISTANT_VERSION_DISPLAY "v${CMAKE_MATCH_1}")
        set(USBASSISTANT_VERSION_IS_RELEASE ON)
    elseif(USBASSISTANT_GIT_DESCRIBE MATCHES
           "^v([0-9]+(\\.[0-9]+)+)-([0-9]+)-g([0-9a-f]+)(-dirty)?$")
        set(USBASSISTANT_VERSION "${CMAKE_MATCH_1}")
        set(USBASSISTANT_VERSION_DISPLAY "dev")
    elseif(USBASSISTANT_GIT_DESCRIBE MATCHES
           "^v([0-9]+(\\.[0-9]+)+)-dirty$")
        set(USBASSISTANT_VERSION "${CMAKE_MATCH_1}")
        set(USBASSISTANT_VERSION_DISPLAY "dev")
    elseif(USBASSISTANT_GIT_DESCRIBE MATCHES "^([0-9a-f]+)(-dirty)?$")
        set(USBASSISTANT_VERSION_DISPLAY "dev")
    endif()
endif()

message(STATUS
    "UsbAssistant version: ${USBASSISTANT_VERSION_DISPLAY} "
    "(source: ${USBASSISTANT_VERSION_SOURCE})")
project(UsbAssistant VERSION ${USBASSISTANT_VERSION} LANGUAGES CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(USBASSISTANT_DEFS
    UNICODE
    _UNICODE
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

# ---------------------------------------------------------------------------
# 核心库：只依赖 Win32 / SetupAPI / CfgMgr32，不包含任何界面代码。
# ---------------------------------------------------------------------------
add_library(usbassistant_core STATIC
    src/core/models.cpp
    src/core/text.cpp
    src/core/log.cpp
    src/core/settings.cpp
    src/core/storage.cpp
    src/core/devices.cpp
    src/core/eject.cpp
    src/core/occupants.cpp
    src/core/elevation.cpp
    src/core/update.cpp
    src/core/http.cpp
    src/core/zip.cpp
    src/core/app_control.cpp
)
target_include_directories(usbassistant_core PUBLIC src)
target_compile_definitions(usbassistant_core PUBLIC ${USBASSISTANT_DEFS}
    "USBASSISTANT_VERSION=L\"${USBASSISTANT_VERSION}\""
    "USBASSISTANT_REPO_URL=L\"https://github.com/JiaHui-Nie/UsbAssistant\""
    $<$<CONFIG:Debug>:USBASSISTANT_DEBUG_BUILD=1>
)
if(USBASSISTANT_VERSION_IS_RELEASE)
    target_compile_definitions(usbassistant_core PUBLIC
        $<$<CONFIG:Debug>:USBASSISTANT_DEV_BUILD=1>)
else()
    # 非 tag 构建（分支、脏工作区）统一按开发版处理。
    target_compile_definitions(usbassistant_core PUBLIC
        USBASSISTANT_DEV_BUILD=1)
endif()
target_link_libraries(usbassistant_core PUBLIC
    setupapi cfgmgr32 shell32 ole32 oleaut32 user32 advapi32 version uuid
    winhttp
)

# ---------------------------------------------------------------------------
# 主程序：同一个 exe 同时承担托盘助手与 UAC 提权助手（--helper）。
# ---------------------------------------------------------------------------
add_executable(UsbAssistant WIN32
    src/main.cpp
    src/app/application.cpp
    src/app/worker.cpp
    src/ui/theme.cpp
    src/ui/canvas.cpp
    src/ui/text.cpp
    src/ui/draw.cpp
    src/ui/main_view.cpp
    src/ui/settings_window.cpp
    src/ui/about_window.cpp
    src/ui/message_dialog.cpp
    src/ui/tray_menu.cpp
    src/ui/edge_bar.cpp
    res/app.rc
)
target_link_libraries(UsbAssistant PRIVATE
    usbassistant_core d2d1 dwrite dwmapi gdi32 gdiplus
)
target_include_directories(UsbAssistant PRIVATE src)

# ---------------------------------------------------------------------------
# 升级器：独立程序，负责检查更新、下载、解压、替换与重启主程序。
# ---------------------------------------------------------------------------
add_executable(UsbAssistantUpdater WIN32
    src/updater/main.cpp
    src/app/updater_app.cpp
    src/app/worker.cpp
    src/ui/theme.cpp
    src/ui/text.cpp
    src/ui/draw.cpp
    src/ui/updater_window.cpp
    res/updater.rc
)
target_link_libraries(UsbAssistantUpdater PRIVATE
    usbassistant_core d2d1 dwrite dwmapi gdi32 gdiplus
)
target_include_directories(UsbAssistantUpdater PRIVATE src)

# ---------------------------------------------------------------------------
# 核心逻辑单元测试（不依赖界面，可用 ctest 运行）。
# ---------------------------------------------------------------------------
option(USBASSISTANT_BUILD_TESTS "Build core unit tests" OFF)
if(USBASSISTANT_BUILD_TESTS)
    add_executable(usbassistant_tests tests/core_tests.cpp)
    target_link_libraries(usbassistant_tests PRIVATE usbassistant_core)
    add_test(NAME core_tests COMMAND usbassistant_tests)
endif()

# ---------------------------------------------------------------------------
# 编译选项 / 运行时库
# ---------------------------------------------------------------------------
if(MSVC)
    foreach(target UsbAssistant UsbAssistantUpdater usbassistant_core)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
    endforeach()
    if(USBASSISTANT_BUILD_TESTS)
        target_compile_options(usbassistant_tests PRIVATE /W4 /utf-8 /permissive-)
    endif()
else()
    foreach(target UsbAssistant UsbAssistantUpdater usbassistant_core)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endforeach()
    if(USBASSISTANT_BUILD_TESTS)
        target_compile_options(usbassistant_tests PRIVATE -Wall -Wextra)
        target_link_options(usbassistant_tests PRIVATE
            -static -static-libgcc -static-libstdc++ -lwinpthread)
    endif()
    target_link_options(UsbAssistant PRIVATE
        -static -static-libgcc -static-libstdc++ -lwinpthread
        $<$<CONFIG:Release>:-s>)
    target_link_options(UsbAssistantUpdater PRIVATE
        -static -static-libgcc -static-libstdc++ -lwinpthread
        $<$<CONFIG:Release>:-s>)
endif()

# ---------------------------------------------------------------------------
# 产物布局（扁平）：
#   x64：cmake -B build -A x64        -> build/Release、build/Debug
#   x86：cmake -B build/x86 -A Win32  -> build/x86/Release、build/x86/Debug
# 多配置生成器会自动追加配置子目录。
# ---------------------------------------------------------------------------
set_target_properties(UsbAssistant PROPERTIES
    OUTPUT_NAME "UsbAssistant"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
set_target_properties(UsbAssistantUpdater PROPERTIES
    OUTPUT_NAME "UsbAssistantUpdater"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(USBASSISTANT_BUILD_TESTS)
    set_target_properties(usbassistant_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()
