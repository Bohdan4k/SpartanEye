cmake_minimum_required(VERSION 3.25)
project(metro_npc_overlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: nothing extra to deploy

set(GAME_BUILD_TIMESTAMP_OVERRIDE "" CACHE STRING "Test builds only: expect this game PE timestamp instead")
if(GAME_BUILD_TIMESTAMP_OVERRIDE)
    add_compile_definitions(GAME_BUILD_TIMESTAMP=${GAME_BUILD_TIMESTAMP_OVERRIDE})
endif()

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)

# MinHook v1.3.4 (vendored, see thirdparty/README.md)
add_library(minhook STATIC
    ${TP}/minhook/src/buffer.c ${TP}/minhook/src/hook.c ${TP}/minhook/src/trampoline.c ${TP}/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${TP}/minhook/include)

# Dear ImGui v1.92.9b core + DX12 renderer backend (vendored). File/clipboard/shell functions compiled out.
add_library(imgui STATIC
    ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_widgets.cpp ${TP}/imgui/imgui_tables.cpp
    ${TP}/imgui/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui PUBLIC ${TP}/imgui ${TP}/imgui/backends)
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_FILE_FUNCTIONS
    IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

add_library(metro_npc_overlay SHARED res/dll.rc
    src/dllmain.cpp src/config.cpp src/log.cpp src/sig.cpp src/game.cpp src/input.cpp src/render.cpp src/report.cpp src/ui.cpp src/capture.cpp src/display.cpp)
target_compile_definitions(metro_npc_overlay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(metro_npc_overlay PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/EHsc>")
target_link_libraries(metro_npc_overlay PRIVATE minhook imgui d3d12 dxgi dinput8 dxguid d3dcompiler)

# Launcher: starts the game through Steam and loads the DLL (shipped as SpartanEye.exe).
add_executable(launcher launcher/launcher.cpp res/launcher.rc)
target_compile_definitions(launcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(launcher PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive->")
target_link_libraries(launcher PRIVATE shell32 ole32 uuid)
set_target_properties(launcher PROPERTIES OUTPUT_NAME SpartanEye)

# Shipped binaries: deterministic (/Brepro: no timestamps), no PDB and so no debug directory / local path inside.
foreach(t metro_npc_overlay launcher)
    target_compile_options(${t} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/Brepro>)
    target_link_options(${t} PRIVATE /Brepro /DEBUG:NONE)
endforeach()
