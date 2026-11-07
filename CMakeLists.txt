cmake_minimum_required(VERSION 3.20)
project(UsbAssistant LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(UsbAssistant WIN32
    src/main.cpp
    src/app.cpp
    src/render.cpp
    src/device.cpp
    src/eject.cpp
    src/canvas.cpp
    src/theme.cpp
    src/settings.cpp
    src/settings_window.cpp
    src/tray_menu.cpp
    src/occupant.cpp
    res/app.rc
)

target_compile_definitions(UsbAssistant PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(UsbAssistant PRIVATE /W3 /utf-8 /permissive-)
    target_link_libraries(UsbAssistant PRIVATE d2d1 dwrite dwmapi shell32 shlwapi setupapi cfgmgr32 ole32 user32 gdi32 advapi32 uuid)
    set_property(TARGET UsbAssistant PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(UsbAssistant PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
    target_link_libraries(UsbAssistant PRIVATE
        d2d1 dwrite dwmapi shell32 shlwapi setupapi cfgmgr32 ole32 user32 gdi32 advapi32 uuid
        gdiplus version
        -static -static-libgcc -static-libstdc++ -lwinpthread)
endif()

set_target_properties(UsbAssistant PROPERTIES
    OUTPUT_NAME "UsbAssistant"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
