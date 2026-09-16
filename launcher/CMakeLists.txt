add_executable(csgo_launcher WIN32
    launcher_win.cpp
    resource.rc
)

set_target_properties(csgo_launcher PROPERTIES
    OUTPUT_NAME "csgo"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL    "${DIST_DIR}"
)

target_link_libraries(csgo_launcher PRIVATE
    user32
    kernel32
)

target_include_directories(csgo_launcher PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)