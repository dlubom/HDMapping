set(_runtime_sources
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
target_link_libraries(processing_session_snapshot PUBLIC processing_result)
list(APPEND _runtime_sources src/ResultPublisher.cpp src/NativeSessionResult.cpp)

set(_step1_dir "${PROJECT_SOURCE_DIR}/apps/lidar_odometry_step_1")
add_library(processing_step1_engine STATIC
    "${_step1_dir}/lidar_odometry.cpp"
    "${_step1_dir}/lidar_odometry_utils.cpp"
    "${_step1_dir}/lidar_odometry_utils_optimizers.cpp"
    "${_step1_dir}/points_global_spill.cpp"
    "${_step1_dir}/raw_cloud_loader.cpp"
    "${_step1_dir}/toml_io.cpp")
target_compile_definitions(processing_step1_engine PRIVATE UTL_PROFILER_DISABLE)
target_compile_definitions(processing_step1_engine PUBLIC WITH_GUI=0)
target_include_directories(processing_step1_engine SYSTEM PUBLIC
    "${PROJECT_SOURCE_DIR}/core/include" "${PROJECT_SOURCE_DIR}/shared/include"
    "${EIGEN3_INCLUDE_DIR}" "${THIRDPARTY_DIRECTORY}" "${THIRDPARTY_DIRECTORY}/tomlplusplus/include"
    "${THIRDPARTY_DIRECTORY}/observation_equations/codes" "${THIRDPARTY_DIRECTORY}/vqf/vqf/cpp"
    "${THIRDPARTY_DIRECTORY}/Fusion/Fusion" "${LASZIP_INCLUDE_DIR}/LASzip/include")
target_link_libraries(processing_step1_engine PUBLIC step1_run_report unordered_dense::unordered_dense
    PRIVATE core_no_gui vqf Fusion spdlog::spdlog UTL::include ${PLATFORM_LASZIP_LIB} ${PLATFORM_MISCELLANEOUS_LIBS})
if(_processing_sanitizer)
    target_compile_options(processing_step1_engine PRIVATE
        -fsanitize=${_processing_sanitizer} -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
endif()
if(MSVC)
    target_compile_definitions(processing_step1_engine PRIVATE _USE_MATH_DEFINES)
    target_compile_options(processing_step1_engine PRIVATE /bigobj)
endif()

set(_step2_dir "${PROJECT_SOURCE_DIR}/apps/multi_view_tls_registration")
set(_step2_sources "${_step2_dir}/step2_operations.cpp" "${_step2_dir}/step2_result.cpp" "${_step2_dir}/step2_state_parameters.cpp")
add_library(processing_step2 STATIC ${_step2_sources})
target_include_directories(processing_step2 PUBLIC "${_step2_dir}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(processing_step2 SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/shared/include" "${THIRDPARTY_DIRECTORY}" "${THIRDPARTY_DIRECTORY}/observation_equations/codes"
    "${LASZIP_INCLUDE_DIR}/LASzip/include")
target_compile_definitions(processing_step2 PRIVATE WITH_GUI=0)
target_link_libraries(processing_step2 PUBLIC processing_session_snapshot processing_result PRIVATE core_no_gui unordered_dense::unordered_dense)
hdmapping_processing_target(processing_step2)
if(MSVC)
    target_compile_definitions(processing_step2 PRIVATE _USE_MATH_DEFINES)
    target_compile_options(processing_step2 PRIVATE /bigobj)
endif()
list(APPEND _runtime_sources ${_step2_sources})

set(_step1_sources src/Operation.cpp "${_step1_dir}/step1_operations.cpp"
    "${_step1_dir}/step1_parameters.cpp" "${_step1_dir}/recording_capture.cpp" "${_step1_dir}/odometry_capture.cpp")
add_library(processing_step1 STATIC ${_step1_sources})
target_include_directories(processing_step1 PUBLIC "${_step1_dir}")
target_include_directories(processing_step1 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(processing_step1 SYSTEM PRIVATE "${THIRDPARTY_DIRECTORY}/json/include")
target_link_libraries(processing_step1 PUBLIC processing_result processing_step1_engine processing_step2)
hdmapping_processing_target(processing_step1)
if(MSVC)
    target_compile_options(processing_step1 PRIVATE /bigobj)
endif()
list(APPEND _runtime_sources ${_step1_sources})

set(_gui_sources src/GuiStateBridge.cpp src/GuiStateConnection.cpp src/GuiRelayProcess.cpp)
add_library(processing_gui_state STATIC ${_gui_sources})
target_include_directories(processing_gui_state SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/shared/include")
target_compile_definitions(processing_gui_state PRIVATE HDMAPPING_GUI_VERSION="${CMAKE_PROJECT_VERSION}")
target_link_libraries(processing_gui_state PUBLIC processing_result)
hdmapping_processing_target(processing_gui_state)
list(APPEND _runtime_sources ${_gui_sources})

set(_gui_capture_sources
    "${PROJECT_SOURCE_DIR}/apps/multi_session_registration/project_capture.cpp")
list(APPEND _runtime_sources ${_gui_capture_sources})

if(BUILD_TESTING)
    add_executable(processing_state_test
        tests/state_revision_test.cpp tests/gui_capture_test.cpp tests/runtime_test_main.cpp ${_gui_capture_sources})
    target_include_directories(processing_state_test SYSTEM PRIVATE
        "${THIRDPARTY_DIRECTORY}/doctest" "${LASZIP_INCLUDE_DIR}/LASzip/include" "${EIGEN3_INCLUDE_DIR}"
        "${PROJECT_SOURCE_DIR}/core/include" "${PROJECT_SOURCE_DIR}/shared/include"
        "${THIRDPARTY_DIRECTORY}" "${THIRDPARTY_DIRECTORY}/observation_equations/codes" "${THIRDPARTY_DIRECTORY}/vqf/vqf/cpp"
        "${PROJECT_SOURCE_DIR}/apps/lidar_odometry_step_1" "${PROJECT_SOURCE_DIR}/apps/multi_session_registration")
    target_compile_definitions(processing_state_test PRIVATE HDMAPPING_GUI_VERSION="${CMAKE_PROJECT_VERSION}")
    target_link_libraries(processing_state_test PRIVATE
        processing_gui_state processing_session_snapshot processing_step1 core_no_gui unordered_dense::unordered_dense)
    if(MSVC)
        target_compile_options(processing_state_test PRIVATE /bigobj)
    endif()
    hdmapping_processing_target(processing_state_test)
    add_test(NAME processing-state COMMAND processing_state_test)
    set_tests_properties(processing-state PROPERTIES LABELS processing-state TIMEOUT 45)
    list(APPEND _runtime_sources tests/state_revision_test.cpp tests/gui_capture_test.cpp
        "${PROJECT_SOURCE_DIR}/apps/multi_view_tls_registration/step2_state_parameters_test.cpp")
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
        "${PROJECT_SOURCE_DIR}/core/include" "${PROJECT_SOURCE_DIR}/shared/include")
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

    add_executable(processing_step1_test tests/step1_operations_test.cpp tests/runtime_test_main.cpp)
    target_include_directories(processing_step1_test SYSTEM PRIVATE "${THIRDPARTY_DIRECTORY}/doctest")
    target_link_libraries(processing_step1_test PRIVATE processing_step1)
    hdmapping_processing_target(processing_step1_test)
    if(MSVC)
        target_compile_options(processing_step1_test PRIVATE /bigobj)
    endif()
    add_test(NAME processing-step1 COMMAND processing_step1_test)
    set_tests_properties(processing-step1 PROPERTIES LABELS processing-step1 TIMEOUT 90)
    list(APPEND _runtime_sources tests/step1_operations_test.cpp)

    add_executable(processing_step2_registration_test tests/step2_registration_test.cpp tests/runtime_test_main.cpp)
    target_include_directories(processing_step2_registration_test SYSTEM PRIVATE "${THIRDPARTY_DIRECTORY}/doctest")
    target_link_libraries(processing_step2_registration_test PRIVATE processing_step1)
    hdmapping_processing_target(processing_step2_registration_test)
    if(MSVC)
        target_compile_options(processing_step2_registration_test PRIVATE /bigobj)
    endif()
    add_test(NAME processing-step2-registration COMMAND processing_step2_registration_test)
    set_tests_properties(processing-step2-registration PROPERTIES LABELS processing-step2-registration TIMEOUT 90)
    list(APPEND _runtime_sources tests/step2_registration_test.cpp)
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
