set(FUNCHOOK_DIR "${CMAKE_SOURCE_DIR}/addons/funchook")
set(STEAM_API_DIR "${CMAKE_SOURCE_DIR}/src/steam")

add_library(csgc SHARED
    main.cpp
    forwarder.cpp
    platform.cpp
    crash_report.cpp
    steam_hook.cpp
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
    # Карта линковки: по RVA из дампа сразу находим имя функции в csgc.dll
    # (tools/crashlab.py map dist/csgc/csgc.map 0x1234).
    target_link_options(csgc PRIVATE "/MAP:${CMAKE_SOURCE_DIR}/dist/csgc/csgc.map")
    # /Oy- — не выбрасывать frame pointer: по дампам получаются нормальные стеки.
    target_compile_options(csgc PRIVATE /Oy-)
    target_link_options(csgc PRIVATE /DEBUG)
endif()

set_target_properties(csgc PROPERTIES
    PREFIX ""
    OUTPUT_NAME "csgc"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/dist/csgc"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/dist/csgc"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/dist/csgc"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL    "${CMAKE_SOURCE_DIR}/dist/csgc"
    PDB_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/csgc"
)
