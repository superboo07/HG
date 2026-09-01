cmake_minimum_required(VERSION 3.24)

project(HauntingGroundRecomp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HG_BUILD_RUNNER "Build the native Haunting Ground runner" ON)

include(CTest)

if(HG_BUILD_RUNNER)
    find_package(OpenGL REQUIRED)

    add_executable(hg_runner
        src/main.cpp
        src/renderer/opengl_renderer.cpp
    )
    target_include_directories(hg_runner PRIVATE src)
    target_link_libraries(hg_runner PRIVATE OpenGL::GL)

    if(MSVC)
        target_compile_options(hg_runner PRIVATE /W4 /permissive-)
    else()
        target_compile_options(hg_runner PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(BUILD_TESTING)
        add_test(NAME hg_runner_smoke COMMAND hg_runner --version)
    endif()
endif()

find_package(Python3 3.11 COMPONENTS Interpreter)
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(
        NAME function_config_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                -s "${CMAKE_SOURCE_DIR}/tests" -p "test_*.py" -v
    )
endif()

# PS2Recomp assumes it is configured as its own source root. Keep that invariant
# instead of reaching into its targets with add_subdirectory().
set(HG_PS2RECOMP_SOURCE "${CMAKE_SOURCE_DIR}/third_party/PS2Recomp")
set(HG_PS2RECOMP_BUILD "${CMAKE_BINARY_DIR}/ps2recomp")
add_custom_target(hg_ps2recomp_configure
    COMMAND "${CMAKE_COMMAND}" -S "${HG_PS2RECOMP_SOURCE}"
            -B "${HG_PS2RECOMP_BUILD}"
            -DPS2X_BUILD_RUNTIME=OFF
            -DPS2X_BUILD_TEST=OFF
            -DPS2X_BUILD_STUDIO=OFF
    COMMENT "Configure pinned PS2Recomp tools"
    VERBATIM
)
add_custom_target(hg_ps2recomp_tools
    COMMAND "${CMAKE_COMMAND}" --build "${HG_PS2RECOMP_BUILD}"
            --config $<CONFIG> --target ps2_recomp ps2_analyzer
    DEPENDS hg_ps2recomp_configure
    COMMENT "Build pinned PS2Recomp recompiler and analyzer"
    VERBATIM
)

