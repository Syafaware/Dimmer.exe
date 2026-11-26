cmake_minimum_required(VERSION 3.20)
project(Dimmer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static-link the C++ runtime so Release/Dimmer.exe runs standalone on any Windows PC
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# WIN32 subsystem creates a native GUI executable with ZERO console window
add_executable(Dimmer WIN32
    src/main.cpp
    src/config.hpp
)

target_compile_definitions(Dimmer PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
)

# Link Direct3D 11, DXGI, HLSL Shader Compiler, and native Win32 libraries
target_link_libraries(Dimmer PRIVATE
    d3d11
    dxgi
    d3dcompiler
    gdi32
    user32
    dwmapi
    uxtheme
)

if(MSVC)
    target_compile_options(Dimmer PRIVATE /W4 /utf-8)
endif()