cmake_minimum_required(VERSION 3.28)
project(rockbottom VERSION 1.0.0 LANGUAGES CXX)

# rockbottom targets the same modern standard maya uses.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ── maya (vendored as a git submodule) ───────────────────────────────────────
# We only want the library, not maya's own examples/tests/fuzzers.
set(MAYA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MAYA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(MAYA_BUILD_FUZZERS  OFF CACHE BOOL "" FORCE)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/maya/CMakeLists.txt")
    message(FATAL_ERROR
        "maya submodule missing — run: git submodule update --init --recursive")
endif()
add_subdirectory(third_party/maya)

# ── rockbottom ──────────────────────────────────────────────────────────────
add_executable(rockbottom
    src/main.cpp
    src/core/sampler.cpp
    src/core/verdict.cpp
    src/core/collectors/cpu.cpp
    src/core/collectors/mem.cpp
    src/core/collectors/disk.cpp
    src/core/collectors/diskio.cpp
    src/core/collectors/net.cpp
    src/core/collectors/gpu.cpp
    src/core/collectors/proc.cpp
    src/core/collectors/ports.cpp
    src/core/collectors/psi.cpp
    src/core/collectors/battery.cpp
)
target_link_libraries(rockbottom PRIVATE maya)
target_compile_features(rockbottom PRIVATE cxx_std_26)

# The binary is `rb` — short, one hand, the way you'll actually type it.
set_target_properties(rockbottom PROPERTIES OUTPUT_NAME rb)

install(TARGETS rockbottom RUNTIME DESTINATION bin)
