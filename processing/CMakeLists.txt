set(_runtime_sources
    src/Operation.cpp
    src/SnapshotStore.cpp
    src/RuntimeProtocol.cpp
    src/RuntimeReceipt.cpp
)
add_library(processing_ops STATIC ${_runtime_sources})
target_include_directories(processing_ops PUBLIC include)
target_include_directories(processing_ops SYSTEM PUBLIC "${THIRDPARTY_DIRECTORY}/json/include")
target_link_libraries(processing_ops PRIVATE Threads::Threads)
hdmapping_processing_target(processing_ops)

add_library(processing_session_snapshot STATIC src/SessionSnapshot.cpp)
target_include_directories(processing_session_snapshot SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/core/include" "${EIGEN3_INCLUDE_DIR}")
target_compile_definitions(processing_session_snapshot PRIVATE WITH_GUI=0)
target_link_libraries(processing_session_snapshot PUBLIC processing_ops)
hdmapping_processing_target(processing_session_snapshot)
list(APPEND _runtime_sources src/SessionSnapshot.cpp)

add_library(processing_result STATIC src/ResultPublisher.cpp src/NativeSessionResult.cpp)
target_include_directories(processing_result SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/core/include" "${PROJECT_SOURCE_DIR}/shared/include" "${EIGEN3_INCLUDE_DIR}")
target_compile_definitions(processing_result PRIVATE WITH_GUI=0)
target_link_libraries(processing_result PUBLIC processing_ops)
hdmapping_processing_target(processing_result)
list(APPEND _runtime_sources src/ResultPublisher.cpp src/NativeSessionResult.cpp)

if(BUILD_TESTING)
    add_executable(processing_runtime_test tests/runtime_test.cpp tests/runtime_test_main.cpp)
    target_include_directories(processing_runtime_test SYSTEM PRIVATE "${THIRDPARTY_DIRECTORY}/doctest")
    target_link_libraries(processing_runtime_test PRIVATE processing_ops)
    hdmapping_processing_target(processing_runtime_test)
    add_test(NAME processing-runtime COMMAND processing_runtime_test)
    set_tests_properties(processing-runtime PROPERTIES LABELS processing-runtime TIMEOUT 45)
    add_executable(processing_snapshot_test tests/snapshot_store_test.cpp tests/snapshot_test_main.cpp)
    target_include_directories(processing_snapshot_test SYSTEM PRIVATE
        "${THIRDPARTY_DIRECTORY}/doctest" "${LASZIP_INCLUDE_DIR}/LASzip/include")
    target_link_libraries(processing_snapshot_test PRIVATE processing_session_snapshot core_no_gui)
    hdmapping_processing_target(processing_snapshot_test)
    add_test(NAME processing-snapshot COMMAND processing_snapshot_test)
    set_tests_properties(processing-snapshot PROPERTIES LABELS processing-snapshot TIMEOUT 45)
    add_executable(processing_publication_test tests/result_publisher_test.cpp)
    target_include_directories(processing_publication_test SYSTEM PRIVATE
        "${THIRDPARTY_DIRECTORY}/doctest" "${LASZIP_INCLUDE_DIR}/LASzip/include" "${EIGEN3_INCLUDE_DIR}"
        "${PROJECT_SOURCE_DIR}/shared/include")
    target_link_libraries(processing_publication_test PRIVATE processing_result core_no_gui)
    hdmapping_processing_target(processing_publication_test)
    add_test(NAME processing-publication COMMAND processing_publication_test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME processing-publication-process
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/result_publication_process.py"
            "$<TARGET_FILE:processing_publication_test>")
    set_tests_properties(processing-publication processing-publication-process
        PROPERTIES LABELS processing-publication TIMEOUT 90)
    list(APPEND _runtime_sources tests/runtime_test.cpp tests/snapshot_store_test.cpp tests/snapshot_test_main.cpp tests/result_publisher_test.cpp)
endif()

if(HDMAPPING_PROCESSING_TIDY)
    add_custom_target(processing-tidy
        COMMAND "${HDMAPPING_PROCESSING_CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" ${_processing_tidy_args}
            "--config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy"
            ${_runtime_sources} "${PROJECT_SOURCE_DIR}/apps/hdmapping_worker/main.cpp"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM USES_TERMINAL)
endif()

if(HDMAPPING_PROCESSING_TIDY OR _processing_sanitizer)
    find_program(HDMAPPING_PROCESSING_CLANG_FORMAT NAMES clang-format-21 clang-format)
    add_custom_target(processing-check-controls
        COMMAND "${CMAKE_COMMAND}"
            "-DHDMAPPING_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DCONTROL_DIR=${CMAKE_CURRENT_BINARY_DIR}/negative-controls"
            "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DBUILD_GENERATOR=${CMAKE_GENERATOR}"
            "-DCLANG_FORMAT=${HDMAPPING_PROCESSING_CLANG_FORMAT}"
            "-DCLANG_TIDY=${HDMAPPING_PROCESSING_CLANG_TIDY}"
            "-DCLANG_TIDY_SDK=${_processing_tidy_sdk}"
            "-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
            "-DSANITIZER=${_processing_sanitizer}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/controls.cmake"
        VERBATIM USES_TERMINAL)
endif()
