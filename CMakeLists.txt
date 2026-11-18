cmake_minimum_required(VERSION 3.21)
project(UsbAssistant LANGUAGES CXX)
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
)
target_include_directories(usbassistant_core PUBLIC src)
target_compile_definitions(usbassistant_core PUBLIC ${USBASSISTANT_DEFS})
target_link_libraries(usbassistant_core PUBLIC
    setupapi cfgmgr32 shell32 ole32 oleaut32 user32 advapi32 version uuid
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
    src/ui/tray_menu.cpp
    src/ui/edge_bar.cpp
    res/app.rc
)
target_link_libraries(UsbAssistant PRIVATE
    usbassistant_core d2d1 dwrite dwmapi gdi32 gdiplus
)
target_include_directories(UsbAssistant PRIVATE src)

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
    foreach(target UsbAssistant usbassistant_core)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
    endforeach()
    if(USBASSISTANT_BUILD_TESTS)
        target_compile_options(usbassistant_tests PRIVATE /W4 /utf-8 /permissive-)
    endif()
else()
    foreach(target UsbAssistant usbassistant_core)
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
endif()

set_target_properties(UsbAssistant PROPERTIES
    OUTPUT_NAME "UsbAssistant"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(USBASSISTANT_BUILD_TESTS)
    set_target_properties(usbassistant_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()
