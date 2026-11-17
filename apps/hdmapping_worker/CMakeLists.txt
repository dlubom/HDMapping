add_executable(hdmapping_worker main.cpp)
target_compile_definitions(hdmapping_worker PRIVATE HDMAPPING_WORKER_VERSION="${PROJECT_VERSION}")
target_link_libraries(hdmapping_worker PRIVATE processing_ops core_no_gui)
hdmapping_processing_target(hdmapping_worker)
hdmapping_install_app(hdmapping_worker)

if(WIN32)
    add_custom_command(TARGET hdmapping_worker POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy -t "$<TARGET_FILE_DIR:hdmapping_worker>" $<TARGET_RUNTIME_DLLS:hdmapping_worker>
        COMMAND_EXPAND_LISTS VERBATIM)
    install(FILES $<TARGET_RUNTIME_DLLS:hdmapping_worker> DESTINATION bin COMPONENT ${HDMAPPING_INSTALL_COMPONENT})
elseif(BUILD_WITH_BUNDLED_LIBLASZIP)
    # The worker is also launched from an installed, relocated runtime.
    install(TARGETS laszip LIBRARY DESTINATION lib COMPONENT ${HDMAPPING_INSTALL_COMPONENT})
    if(NOT APPLE)
        set_property(TARGET hdmapping_worker APPEND PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
    endif()
endif()
