cmake_minimum_required(VERSION 3.24)

project(CodexTaskbar VERSION 0.2.6 LANGUAGES CXX)

add_executable(CodexTaskbar WIN32
    src/main.cpp
    src/AppServerClient.cpp
    src/DesktopThreadMonitor.cpp
    src/SimpleJson.cpp
    src/CodexTaskbar.rc
)

target_compile_features(CodexTaskbar PRIVATE cxx_std_20)

target_compile_definitions(CodexTaskbar PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(CodexTaskbar PRIVATE
        /W4
        /permissive-
        /utf-8
        /EHsc
    )
endif()

target_link_libraries(CodexTaskbar PRIVATE
    user32
    gdi32
    dwmapi
    uxtheme
    shell32
    advapi32
    comctl32
)

set_target_properties(CodexTaskbar PROPERTIES
    OUTPUT_NAME "CodexTaskbar"
)
