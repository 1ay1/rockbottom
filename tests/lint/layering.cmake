# tests/lint/layering.cmake: rb -> maya -> jaal, strictly.
#
# rb only uses maya. jaal is maya's runtime, reached through maya's seam
# (<maya/runtime.hpp>, maya::Cmd/Sub/Sink, MAYA_SENDABLE). Comments may name
# jaal; code and build files may not.
#
# Run with: cmake -DROOT=<rb source dir> -P layering.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

set(hits "")
file(GLOB_RECURSE files ${ROOT}/src/*.cpp ${ROOT}/src/*.hpp ${ROOT}/src/*.h
                        ${ROOT}/tests/*.cpp ${ROOT}/tests/*.hpp)
foreach(f IN LISTS files)
    file(STRINGS ${f} lines REGEX "jaal|JAAL")
    file(RELATIVE_PATH rel ${ROOT} ${f})
    foreach(l IN LISTS lines)
        string(REGEX REPLACE "//.*$" "" code "${l}")
        string(REGEX REPLACE "\"[^\"]*\"" "\"\"" code "${code}")
        if(code MATCHES "#[ \t]*include[ \t]*[<\"]jaal/"
           OR code MATCHES "(^|[^A-Za-z0-9_])jaal::"
           OR code MATCHES "(^|[^A-Za-z0-9_])JAAL_[A-Z]")
            string(STRIP "${l}" s)
            list(APPEND hits "${rel}: ${s}")
        endif()
    endforeach()
endforeach()

# The build may link maya::app / maya, never jaal directly.
file(STRINGS ${ROOT}/CMakeLists.txt lines REGEX "jaal")
foreach(l IN LISTS lines)
    string(REGEX REPLACE "#.*$" "" code "${l}")
    string(REGEX REPLACE "\"[^\"]*\"" "\"\"" code "${code}")
    if(code MATCHES "jaal")
        string(STRIP "${l}" s)
        list(APPEND hits "CMakeLists.txt: ${s}")
    endif()
endforeach()

if(hits)
    list(JOIN hits "\n  " msg)
    message(FATAL_ERROR
        "rb reaches past maya into jaal:\n  ${msg}\n\n"
        "Use the maya:: name from <maya/runtime.hpp>. If maya doesn't export "
        "what you need, add it to maya's seam first.")
endif()
message(STATUS "layering: ok (rb -> maya -> jaal)")
