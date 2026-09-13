cmake_minimum_required(VERSION 3.20)
project(xpt C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "GTA:SA needs 32-bit build! Use: cmake -B build -A Win32")
endif()

add_library(xpt SHARED
    src/main.cpp
    thirdparty/kthook/kthook/hde/hde32.c
    thirdparty/imgui/imgui.cpp
    thirdparty/imgui/imgui_draw.cpp
    thirdparty/imgui/imgui_tables.cpp
    thirdparty/imgui/imgui_widgets.cpp
    thirdparty/imgui/imgui_impl_dx9.cpp
    thirdparty/imgui/imgui_impl_win32.cpp
)

set_target_properties(xpt PROPERTIES
    SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "C:/Users/gryna/OneDrive/Desktop/arizona"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "C:/Users/gryna/OneDrive/Desktop/arizona"
)

target_include_directories(xpt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/kthook/kthook/include
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/kthook/kthook
)

target_compile_definitions(xpt PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(xpt PRIVATE /EHsc /W3)
    set_property(TARGET xpt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(xpt PRIVATE d3d9 user32)
