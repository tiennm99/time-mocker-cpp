cmake_minimum_required(VERSION 3.20)
project(TimeMocker CXX)

find_package(detours CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only.")
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/TimeMocker.UI/imgui")

# ── TimeMocker.Hook (DLL) ─────────────────────────────────────────────────────
add_library(TimeMocker.Hook SHARED
    TimeMocker.Hook/dllmain.cpp
    TimeMocker.Hook/exports.cpp
    TimeMocker.Hook/TimeMocker.Hook.def
)
target_include_directories(TimeMocker.Hook PRIVATE Shared)
target_link_libraries(TimeMocker.Hook PRIVATE detours::detours)
set_target_properties(TimeMocker.Hook PROPERTIES OUTPUT_NAME "TimeMocker.Hook"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ── ImGui static lib ──────────────────────────────────────────────────────────
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_win32.cpp
    ${IMGUI_DIR}/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})

# ── TimeMocker.UI (GUI exe) ───────────────────────────────────────────────────
add_executable(TimeMocker.UI WIN32
    TimeMocker.UI/main.cpp
    TimeMocker.Injector/InjectionManager.cpp
)
target_include_directories(TimeMocker.UI PRIVATE Shared TimeMocker.Injector ${IMGUI_DIR})
target_link_libraries(TimeMocker.UI PRIVATE imgui detours::detours d3d11 dxgi Psapi)
set_target_properties(TimeMocker.UI PROPERTIES OUTPUT_NAME "TimeMocker"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_custom_command(TARGET TimeMocker.UI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:TimeMocker.Hook>
        "${CMAKE_BINARY_DIR}/bin/TimeMocker.Hook.x64.dll"
    COMMENT "Copying Hook DLL")

install(TARGETS TimeMocker.UI TimeMocker.Hook RUNTIME DESTINATION bin)
