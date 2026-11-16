include(${PROJECT_SOURCE_DIR}/cmake/go.cmake)

option(HDMAPPING_MCP_QUALITY "Enable pinned Go developer checks (requires Python 3)" OFF)
option(HDMAPPING_MCP_RACE "Enable Go race tests (requires cgo and a C compiler)" OFF)

set(_mcp_output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND _mcp_output_dir "/$<CONFIG>")
endif()
set(_mcp_output "${_mcp_output_dir}/hdmapping_mcp${CMAKE_EXECUTABLE_SUFFIX}")
file(GLOB_RECURSE _mcp_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmd/*.go" "${CMAKE_CURRENT_SOURCE_DIR}/internal/*.go")
set(_mcp_version_flag "-X=main.productVersion=${PROJECT_VERSION}")
# Use Go's own incremental cache. A file dependency on an imported executable
# can be resolved by CMake as a target dependency and lose its generating rule.
add_custom_target(hdmapping_mcp_build ALL
    BYPRODUCTS "${_mcp_output}"
    COMMAND "${CMAKE_COMMAND}" -E env ${HDMAPPING_GO_ENV}
        "${HDMAPPING_GO}" build -trimpath -buildvcs=false "-ldflags=${_mcp_version_flag}" -o "${_mcp_output}" ./cmd/mcp
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS ${_mcp_sources} go.mod go.sum "${PROJECT_SOURCE_DIR}/CMakeLists.txt"
    COMMENT "Building HDMapping MCP ${PROJECT_VERSION}" VERBATIM)
add_executable(hdmapping_mcp IMPORTED GLOBAL)
set_target_properties(hdmapping_mcp PROPERTIES IMPORTED_LOCATION "${_mcp_output}")
# Imported locations cannot contain generator expressions on all CMake generators.
if(CMAKE_CONFIGURATION_TYPES)
    foreach(_config IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${_config}" _config_upper)
        set_property(TARGET hdmapping_mcp PROPERTY "IMPORTED_LOCATION_${_config_upper}"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_config}/hdmapping_mcp${CMAKE_EXECUTABLE_SUFFIX}")
    endforeach()
else()
    set_property(TARGET hdmapping_mcp PROPERTY IMPORTED_LOCATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/hdmapping_mcp${CMAKE_EXECUTABLE_SUFFIX}")
endif()
add_dependencies(hdmapping_mcp hdmapping_mcp_build)
hdmapping_install_app(hdmapping_mcp)

if(BUILD_TESTING)
    add_test(NAME mcp-contract COMMAND "${CMAKE_COMMAND}" -E env ${HDMAPPING_GO_ENV}
        "HDMAPPING_MCP_EXECUTABLE=$<TARGET_FILE:hdmapping_mcp>"
        "${HDMAPPING_GO}" test -vet=off -count=1 -timeout=90s ./...)
    set_tests_properties(mcp-contract PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" LABELS "mcp" TIMEOUT 120)
endif()
if(HDMAPPING_MCP_QUALITY)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_mcp_checks "${PROJECT_SOURCE_DIR}/scripts/run_mcp_checks.py")
    set(_mcp_check_args --go "${HDMAPPING_GO}" --tools-dir "${CMAKE_BINARY_DIR}/go-tools")
    add_custom_target(mcp-check
        COMMAND "${CMAKE_COMMAND}" -E env ${HDMAPPING_GO_ENV}
            "${Python3_EXECUTABLE}" "${_mcp_checks}" ${_mcp_check_args} --checks format lint test fuzz vuln
        USES_TERMINAL VERBATIM)
    set(_mcp_race_control "")
    if(HDMAPPING_MCP_RACE)
        set(_mcp_race_control --race-controls)
    endif()
    add_custom_target(mcp-check-controls
        COMMAND "${CMAKE_COMMAND}" -E env ${HDMAPPING_GO_ENV}
            "${Python3_EXECUTABLE}" "${_mcp_checks}" ${_mcp_check_args} --checks controls ${_mcp_race_control}
        USES_TERMINAL VERBATIM)
endif()
if(HDMAPPING_MCP_RACE)
    add_custom_target(mcp-race
        COMMAND "${CMAKE_COMMAND}" -E env ${HDMAPPING_GO_ENV} CGO_ENABLED=1
            "${HDMAPPING_GO}" test -vet=off -race -count=1 -timeout=90s ./...
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL VERBATIM)
endif()
