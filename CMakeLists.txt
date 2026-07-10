cmake_minimum_required(VERSION 3.28)
project(bottom VERSION 1.0.0 LANGUAGES CXX)

# bottom targets the same modern standard maya uses.
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

# ── bottom ─────────────────────────────────────────────────────────────────────────────
add_executable(bottom
    src/main.cpp
    src/core/sampler.cpp
    src/core/verdict.cpp
    src/core/collectors/cpu.cpp
    src/core/collectors/mem.cpp
    src/core/collectors/disk.cpp
    src/core/collectors/net.cpp
    src/core/collectors/proc.cpp
)
target_link_libraries(bottom PRIVATE maya)
target_compile_features(bottom PRIVATE cxx_std_26)

install(TARGETS bottom RUNTIME DESTINATION bin)
