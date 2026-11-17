include(${PROJECT_SOURCE_DIR}/cmake/go.cmake)

option(HDMAPPING_MCP_QUALITY "Enable pinned Go developer checks (requires Python 3)" OFF)
option(HDMAPPING_MCP_RACE "Enable Go race tests (requires cgo and a C compiler)" OFF)

set(_mcp_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND _mcp_output_dir "/$<CONFIG>")
endif()
file(GLOB_RECURSE _mcp_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmd/*.go" "${CMAKE_CURRENT_SOURCE_DIR}/internal/*.go")
set(_mcp_version_flag "-X=main.productVersion=${PROJECT_VERSION}")

function(hdmapping_go_app target package)
    set(output "${_mcp_output_dir}/${target}${CMAKE_EXECUTABLE_SUFFIX}")
    # Go's incremental cache preserves its own dependency graph.
    add_custom_target(${target}_build ALL
        BYPRODUCTS "${output}"
        COMMAND "${CMAKE_COMMAND}" -E env ${HDMAPPING_GO_ENV}
            "${HDMAPPING_GO}" build -trimpath -buildvcs=false "-ldflags=${_mcp_version_flag}" -o "${output}" "${package}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS ${_mcp_sources} go.mod go.sum "${PROJECT_SOURCE_DIR}/CMakeLists.txt"
        COMMENT "Building ${target} ${PROJECT_VERSION}" VERBATIM)
    add_executable(${target} IMPORTED GLOBAL)
    if(CMAKE_CONFIGURATION_TYPES)
        foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${config}" config_upper)
            set_property(TARGET ${target} PROPERTY "IMPORTED_LOCATION_${config_upper}"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}/${target}${CMAKE_EXECUTABLE_SUFFIX}")
        endforeach()
    else()
        set_property(TARGET ${target} PROPERTY IMPORTED_LOCATION "${output}")
    endif()
    add_dependencies(${target} ${target}_build)
    hdmapping_install_app(${target})
endfunction()

hdmapping_go_app(hdmapping_mcp ./cmd/mcp)
hdmapping_go_app(hdmapping_coordinator ./cmd/coordinator)
add_dependencies(hdmapping_mcp_build hdmapping_coordinator_build hdmapping_worker)

if(BUILD_TESTING)
    add_test(NAME mcp-contract COMMAND "${HDMAPPING_GO}" test -vet=off -v -count=1 -timeout=90s ./...)
    set_tests_properties(mcp-contract PROPERTIES
        ENVIRONMENT "${HDMAPPING_GO_ENV};HDMAPPING_MCP_EXECUTABLE=$<TARGET_FILE:hdmapping_mcp>"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" LABELS "mcp" TIMEOUT 120)
endif()
if(HDMAPPING_MCP_QUALITY)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_mcp_checks "${PROJECT_SOURCE_DIR}/scripts/run_mcp_checks.py")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/mcp-go-environment.txt"
        CONTENT "$<JOIN:${HDMAPPING_GO_ENV},\n>\n")
    set(_mcp_check_args --build-dir "${CMAKE_BINARY_DIR}")
    add_custom_target(mcp-check
        COMMAND "${Python3_EXECUTABLE}" "${_mcp_checks}" ${_mcp_check_args} --checks format lint test fuzz vuln
        USES_TERMINAL VERBATIM)
    set(_mcp_race_control "")
    if(HDMAPPING_MCP_RACE)
        set(_mcp_race_control --race-controls)
    endif()
    add_custom_target(mcp-check-controls
        COMMAND "${Python3_EXECUTABLE}" "${_mcp_checks}" ${_mcp_check_args} --checks controls ${_mcp_race_control}
        USES_TERMINAL VERBATIM)
endif()
if(HDMAPPING_MCP_RACE)
    add_custom_target(mcp-race
        COMMAND "${CMAKE_COMMAND}" -E env ${HDMAPPING_GO_ENV} CGO_ENABLED=1
            "${HDMAPPING_GO}" test -vet=off -race -count=1 -timeout=90s ./...
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL VERBATIM)
endif()
