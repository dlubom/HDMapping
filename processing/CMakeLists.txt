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
target_include_directories(processing_session_snapshot SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/shared/include")
target_include_directories(processing_session_snapshot SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/core/include" "${EIGEN3_INCLUDE_DIR}")
target_compile_definitions(processing_session_snapshot PRIVATE WITH_GUI=0)
target_link_libraries(processing_session_snapshot PUBLIC processing_ops)
hdmapping_processing_target(processing_session_snapshot)
list(APPEND _runtime_sources src/SessionSnapshot.cpp)

set(_step3_dir "${PROJECT_SOURCE_DIR}/apps/multi_session_registration")
set(_native_result_sources src/ResultPublisher.cpp src/NativeSessionResult.cpp src/NativeSessionWriter.cpp
    src/NativeProjectResult.cpp "${_step3_dir}/project_io.cpp")
add_library(processing_result STATIC ${_native_result_sources})
target_include_directories(processing_result PRIVATE "${_step3_dir}")
target_include_directories(processing_result SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/core/include" "${PROJECT_SOURCE_DIR}/shared/include" "${EIGEN3_INCLUDE_DIR}"
    "${LASZIP_INCLUDE_DIR}/LASzip/include")
target_compile_definitions(processing_result PRIVATE WITH_GUI=0)
target_link_libraries(processing_result PUBLIC processing_ops)
hdmapping_processing_target(processing_result)
target_link_libraries(processing_session_snapshot PUBLIC processing_result)
list(APPEND _runtime_sources ${_native_result_sources})
if(MSVC)
    target_compile_options(processing_result PRIVATE /bigobj)
endif()

set(_project_sources "${_step3_dir}/project_operations.cpp" "${_step3_dir}/project_capture.cpp"
    "${_step3_dir}/step3_operations.cpp" "${_step3_dir}/multi_session_factor_graph.cpp"
    "${_step3_dir}/step3_io_operations.cpp" "${_step3_dir}/step3_export_formats.cpp")
add_library(processing_project STATIC ${_project_sources})
target_include_directories(processing_project PUBLIC "${_step3_dir}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(processing_project SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/shared/include"
    "${THIRDPARTY_DIRECTORY}/observation_equations/codes" "${LASZIP_INCLUDE_DIR}/LASzip/include")
target_compile_definitions(processing_project PRIVATE WITH_GUI=0)
target_link_libraries(processing_project PUBLIC processing_session_snapshot PRIVATE core_math)
hdmapping_processing_target(processing_project)
if(MSVC)
    target_compile_options(processing_project PRIVATE /bigobj)
    target_compile_definitions(processing_project PRIVATE _USE_MATH_DEFINES)
endif()
list(APPEND _runtime_sources ${_project_sources})

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
add_library(processing_constraints STATIC "${_step2_dir}/step2_constraint_operations.cpp")
target_include_directories(processing_constraints PUBLIC "${_step2_dir}")
target_include_directories(processing_constraints SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/shared/include" "${THIRDPARTY_DIRECTORY}")
target_compile_definitions(processing_constraints PRIVATE WITH_GUI=0)
if(MSVC)
    target_compile_definitions(processing_constraints PRIVATE _USE_MATH_DEFINES)
endif()
target_link_libraries(processing_constraints PUBLIC processing_session_snapshot PRIVATE core_math)
hdmapping_processing_target(processing_constraints)
list(APPEND _runtime_sources "${_step2_dir}/step2_constraint_operations.cpp")
add_library(processing_georeference OBJECT "${_step2_dir}/step2_georeference_operations.cpp")
target_include_directories(processing_georeference PUBLIC "${_step2_dir}")
target_include_directories(processing_georeference SYSTEM PRIVATE "${THIRDPARTY_DIRECTORY}")
target_compile_definitions(processing_georeference PRIVATE WITH_GUI=0)
if(MSVC)
    target_compile_definitions(processing_georeference PRIVATE _USE_MATH_DEFINES)
endif()
target_link_libraries(processing_georeference PUBLIC processing_session_snapshot PRIVATE core_math)
hdmapping_processing_target(processing_georeference)
list(APPEND _runtime_sources "${_step2_dir}/step2_georeference_operations.cpp")
set(_step2_sources "${_step2_dir}/step2_operations.cpp" "${_step2_dir}/step2_result.cpp" "${_step2_dir}/step2_state_parameters.cpp"
    "${_step2_dir}/step2_segment_operations.cpp" "${_step2_dir}/step2_io_operations.cpp" "${_step2_dir}/step2_session_exports.cpp"
    "${_step2_dir}/step2_loop_candidate_operations.cpp" "${_step2_dir}/bow_loop_closure_detector.cpp")
add_library(processing_step2 STATIC ${_step2_sources})
target_include_directories(processing_step2 PUBLIC "${_step2_dir}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(processing_step2 SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/shared/include" "${THIRDPARTY_DIRECTORY}" "${THIRDPARTY_DIRECTORY}/observation_equations/codes"
    "${LASZIP_INCLUDE_DIR}/LASzip/include" "${FBOW_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_compile_definitions(processing_step2 PRIVATE WITH_GUI=0)
target_link_libraries(processing_step2 PUBLIC processing_constraints processing_result PRIVATE processing_georeference core_no_gui unordered_dense::unordered_dense wgs84_do_puwg92 fbow ${OpenCV_LIBS} spdlog::spdlog)
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
target_link_libraries(processing_step1 PUBLIC processing_result processing_step1_engine processing_step2 processing_project)
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

if(BUILD_TESTING)
    add_executable(processing_state_test
        tests/state_revision_test.cpp tests/gui_capture_test.cpp tests/runtime_test_main.cpp)
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

    add_executable(processing_constraints_test tests/constraint_operations_test.cpp tests/runtime_test_main.cpp)
    target_include_directories(processing_constraints_test SYSTEM PRIVATE "${THIRDPARTY_DIRECTORY}/doctest")
    target_link_libraries(processing_constraints_test PRIVATE processing_step1)
    hdmapping_processing_target(processing_constraints_test)
    if(MSVC)
        target_compile_options(processing_constraints_test PRIVATE /bigobj)
    endif()
    add_test(NAME processing-constraints COMMAND processing_constraints_test)
    set_tests_properties(processing-constraints PROPERTIES LABELS processing-constraints TIMEOUT 90)
    list(APPEND _runtime_sources tests/constraint_operations_test.cpp)
    add_executable(processing_georeference_test tests/georeference_operations_test.cpp tests/runtime_test_main.cpp)
    target_include_directories(processing_georeference_test SYSTEM PRIVATE "${THIRDPARTY_DIRECTORY}/doctest")
    target_include_directories(processing_georeference_test PRIVATE "${_step2_dir}")
    target_link_libraries(processing_georeference_test PRIVATE processing_step1)
    if(MSVC)
        target_compile_options(processing_georeference_test PRIVATE /bigobj)
    endif()
    hdmapping_processing_target(processing_georeference_test)
    add_test(NAME processing-georeference COMMAND processing_georeference_test)
    set_tests_properties(processing-georeference PROPERTIES LABELS processing-georeference TIMEOUT 90)
    list(APPEND _runtime_sources tests/georeference_operations_test.cpp)

    add_executable(processing_loop_candidates_test tests/loop_candidates_test.cpp tests/runtime_test_main.cpp)
    target_include_directories(processing_loop_candidates_test SYSTEM PRIVATE
        "${THIRDPARTY_DIRECTORY}/doctest" "${PROJECT_SOURCE_DIR}/shared/include" ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(processing_loop_candidates_test PRIVATE processing_step1 ${OpenCV_LIBS})
    add_dependencies(processing_loop_candidates_test hdmapping_bow_vocabulary)
    if(MSVC)
        target_compile_options(processing_loop_candidates_test PRIVATE /bigobj)
    endif()
    hdmapping_processing_target(processing_loop_candidates_test)
    add_test(NAME processing-loop-candidates COMMAND processing_loop_candidates_test)
    set_tests_properties(processing-loop-candidates PROPERTIES LABELS processing-loop-candidates TIMEOUT 90)
    if(_processing_sanitizer STREQUAL "thread")
        target_compile_definitions(processing_loop_candidates_test PRIVATE HDMAPPING_LOOP_CANDIDATES_TSAN)
        set_property(TEST processing-loop-candidates APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "TSAN_OPTIONS=string_append: suppressions=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/loop_candidates_tsan.supp\"")
    endif()
    list(APPEND _runtime_sources tests/loop_candidates_test.cpp)

    add_executable(processing_segments_test tests/segment_operations_test.cpp tests/runtime_test_main.cpp)
    target_include_directories(processing_segments_test SYSTEM PRIVATE "${THIRDPARTY_DIRECTORY}/doctest")
    target_link_libraries(processing_segments_test PRIVATE processing_step1 processing_gui_state)
    if(MSVC)
        target_compile_options(processing_segments_test PRIVATE /bigobj)
    endif()
    hdmapping_processing_target(processing_segments_test)
    add_test(NAME processing-segments COMMAND processing_segments_test)
    set_tests_properties(processing-segments PROPERTIES LABELS processing-segments TIMEOUT 90)
    list(APPEND _runtime_sources tests/segment_operations_test.cpp)
    add_executable(processing_session_io_test tests/session_io_operations_test.cpp tests/runtime_test_main.cpp)
    target_include_directories(processing_session_io_test SYSTEM PRIVATE
        "${THIRDPARTY_DIRECTORY}/doctest" "${PROJECT_SOURCE_DIR}/shared/include" "${LASZIP_INCLUDE_DIR}/LASzip/include")
    target_link_libraries(processing_session_io_test PRIVATE processing_step1 core_no_gui)
    if(MSVC)
        target_compile_options(processing_session_io_test PRIVATE /bigobj)
    endif()
    hdmapping_processing_target(processing_session_io_test)
    add_test(NAME processing-session-io COMMAND processing_session_io_test)
    set_tests_properties(processing-session-io PROPERTIES LABELS processing-session-io TIMEOUT 90)
    list(APPEND _runtime_sources tests/session_io_operations_test.cpp)

    add_executable(processing_project_io_test tests/project_io_test.cpp tests/runtime_test_main.cpp)
    target_include_directories(processing_project_io_test SYSTEM PRIVATE "${THIRDPARTY_DIRECTORY}/doctest" "${LASZIP_INCLUDE_DIR}/LASzip/include")
    target_link_libraries(processing_project_io_test PRIVATE processing_step1 core_no_gui)
    if(MSVC)
        target_compile_options(processing_project_io_test PRIVATE /bigobj)
    endif()
    hdmapping_processing_target(processing_project_io_test)
    add_test(NAME processing-project-io COMMAND processing_project_io_test --test-case-exclude=P16*)
    add_test(NAME processing-project-export COMMAND processing_project_io_test --test-case=P16*)
    set_tests_properties(processing-project-export PROPERTIES LABELS processing-project-export TIMEOUT 90)
    set_tests_properties(processing-project-io PROPERTIES LABELS processing-project-io TIMEOUT 90)
    list(APPEND _runtime_sources tests/project_io_test.cpp)

    add_executable(processing_step3_test tests/step3_operations_test.cpp tests/runtime_test_main.cpp)
    target_include_directories(processing_step3_test SYSTEM PRIVATE "${THIRDPARTY_DIRECTORY}/doctest")
    target_link_libraries(processing_step3_test PRIVATE processing_step1 core_no_gui)
    if(MSVC)
        target_compile_options(processing_step3_test PRIVATE /bigobj)
    endif()
    hdmapping_processing_target(processing_step3_test)
    add_test(NAME processing-step3 COMMAND processing_step3_test)
    set_tests_properties(processing-step3 PROPERTIES LABELS processing-step3 TIMEOUT 90)
    list(APPEND _runtime_sources tests/step3_operations_test.cpp)
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
