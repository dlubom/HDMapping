# Isolated negative controls. Success requires the intended diagnostic from each tool.
function(expect_failure name pattern)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status OUTPUT_VARIABLE log ERROR_VARIABLE errors)
    if(status STREQUAL "0" OR NOT "${log}\n${errors}" MATCHES "${pattern}")
        message(FATAL_ERROR "${name} control did not report the intended error (${status}):\n${log}\n${errors}")
    endif()
    message(STATUS "PASS negative control: ${name} (${status})")
endfunction()

file(MAKE_DIRECTORY "${CONTROL_DIR}")

if(CLANG_TIDY)
    if(NOT CLANG_FORMAT)
        message(FATAL_ERROR "Processing negative controls need clang-format 21")
    endif()
    execute_process(COMMAND "${CLANG_FORMAT}" --version OUTPUT_VARIABLE formatter_version COMMAND_ERROR_IS_FATAL ANY)
    if(NOT formatter_version MATCHES "version 21[.]")
        message(FATAL_ERROR "Expected clang-format 21, got ${formatter_version}")
    endif()

    file(WRITE "${CONTROL_DIR}/probe.cpp" "int main(){int* p=nullptr; return *p;}\n")
    file(WRITE "${CONTROL_DIR}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 4.0)
project(processing_controls LANGUAGES CXX)
add_executable(probe probe.cpp)
set_target_properties(probe PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF)
enable_testing()
add_test(NAME intentional-failure COMMAND ${CMAKE_COMMAND} -E false)
]=])
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${CONTROL_DIR}" -B "${CONTROL_DIR}/build" -G "${BUILD_GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON COMMAND_ERROR_IS_FATAL ANY)
    expect_failure(clang-format "code should be clang-formatted"
        "${CLANG_FORMAT}" --dry-run --Werror "--style=file:${HDMAPPING_SOURCE_DIR}/.clang-format" "${CONTROL_DIR}/probe.cpp")
    if(CLANG_TIDY_SDK)
        set(tidy_args "--extra-arg=-isysroot" "--extra-arg=${CLANG_TIDY_SDK}")
    endif()
    expect_failure(clang-tidy "clang-analyzer-core[.]NullDereference"
        "${CLANG_TIDY}" -p "${CONTROL_DIR}/build" ${tidy_args} "--config-file=${HDMAPPING_SOURCE_DIR}/processing/.clang-tidy"
        "${CONTROL_DIR}/probe.cpp")
    expect_failure(ctest "intentional-failure [(]Failed[)]"
        "${CTEST_COMMAND}" --test-dir "${CONTROL_DIR}/build" --output-on-failure)
endif()

function(sanitizer_probe name source pattern)
    set(file "${CONTROL_DIR}/${name}.cpp")
    set(binary "${CONTROL_DIR}/${name}")
    file(WRITE "${file}" "${source}")
    execute_process(COMMAND "${CXX_COMPILER}" -std=c++20 -g -O1 -pthread
        "-fsanitize=${SANITIZER}" -fno-sanitize-recover=all -fno-omit-frame-pointer
        "${file}" -o "${binary}" COMMAND_ERROR_IS_FATAL ANY)
    expect_failure("${name}" "${pattern}" "${binary}" trigger)
endfunction()

if(SANITIZER STREQUAL "address,undefined")
    sanitizer_probe(asan [=[
int main(int argc, char**) {
    volatile int index = argc;
    auto* values = new int[1];
    values[index] = 42;
    const int result = values[index];
    delete[] values;
    return result;
}
]=] "AddressSanitizer: heap-buffer-overflow")
    sanitizer_probe(ubsan [=[
#include <climits>
int main(int argc, char**) {
    volatile int high = INT_MAX;
    return high + argc;
}
]=] "runtime error: signed integer overflow")
elseif(SANITIZER STREQUAL "thread")
    sanitizer_probe(tsan [=[
#include <barrier>
#include <thread>
int main() {
    volatile int value = 0;
    std::barrier gate(2);
    auto race = [&] {
        gate.arrive_and_wait();
        for (int i = 0; i < 10000; ++i) {
            value = value + 1;
        }
    };
    std::thread first(race), second(race);
    first.join();
    second.join();
    return value == 0;
}
]=] "ThreadSanitizer: data race")
endif()
