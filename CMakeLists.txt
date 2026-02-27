cmake_minimum_required(VERSION 3.20)
project(TimeMocker CXX)

# ── vcpkg toolchain (set via: cmake -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake)
find_package(detours CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Require Windows
if (NOT WIN32)
    message(FATAL_ERROR "TimeMocker is Windows-only (uses Win32 API and MS Detours).")
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

# ── Shared headers ────────────────────────────────────────────────────────────
set(SHARED_DIR ${CMAKE_SOURCE_DIR}/Shared)

# ── TimeMocker.Hook (DLL) ─────────────────────────────────────────────────────
add_library(TimeMocker.Hook SHARED
    TimeMocker.Hook/dllmain.cpp
    TimeMocker.Hook/exports.cpp
    TimeMocker.Hook/TimeMocker.Hook.def
)
target_include_directories(TimeMocker.Hook PRIVATE ${SHARED_DIR})
target_link_libraries(TimeMocker.Hook PRIVATE detours::detours)
set_target_properties(TimeMocker.Hook PROPERTIES
    OUTPUT_NAME "TimeMocker.Hook"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ── TimeMocker.Injector (static lib) ─────────────────────────────────────────
add_library(TimeMocker.Injector STATIC
    TimeMocker.Injector/InjectionManager.cpp
)
target_include_directories(TimeMocker.Injector
    PUBLIC  ${CMAKE_SOURCE_DIR}/TimeMocker.Injector
    PRIVATE ${SHARED_DIR}
)
target_link_libraries(TimeMocker.Injector PUBLIC detours::detours Psapi)

# ── TimeMocker.UI (console exe) ───────────────────────────────────────────────
add_executable(TimeMocker.UI
    TimeMocker.UI/main.cpp
)
target_include_directories(TimeMocker.UI PRIVATE ${SHARED_DIR})
target_link_libraries(TimeMocker.UI PRIVATE TimeMocker.Injector detours::detours)
set_target_properties(TimeMocker.UI PROPERTIES
    OUTPUT_NAME "TimeMocker"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Copy hook DLLs next to the UI executable after build
add_custom_command(TARGET TimeMocker.UI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:TimeMocker.Hook>
        "${CMAKE_BINARY_DIR}/bin/TimeMocker.Hook.x64.dll"
    COMMENT "Copying Hook DLL (x64) → bin/"
)

install(TARGETS TimeMocker.UI TimeMocker.Hook
        RUNTIME DESTINATION bin)
