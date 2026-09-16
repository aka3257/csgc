set(FUNCHOOK_DIR "${CMAKE_SOURCE_DIR}/addons/funchook")
set(STEAM_API_DIR "${CMAKE_SOURCE_DIR}/src/steam")

add_library(csgc SHARED
    main.cpp
    forwarder.cpp
    platform.cpp
    steam_hook_lite.cpp
)

target_include_directories(csgc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${FUNCHOOK_DIR}/include"
    "${STEAM_API_DIR}/public"
)

if(WIN32)
    target_compile_definitions(csgc PRIVATE PSAPI_VERSION=1)
    target_link_libraries(csgc PRIVATE
        "${FUNCHOOK_DIR}/lib/funchook.lib"
        "${FUNCHOOK_DIR}/lib/distorm.lib"
        "${STEAM_API_DIR}/lib/steam_api.lib"
        Psapi.lib
        ws2_32
    )
endif()

set_target_properties(csgc PROPERTIES
    PREFIX ""
    OUTPUT_NAME "csgc"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/dist/csgc"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/dist/csgc"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/dist/csgc"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL    "${CMAKE_SOURCE_DIR}/dist/csgc"
)