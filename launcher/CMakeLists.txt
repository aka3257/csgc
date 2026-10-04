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

# ВАЖНО: размер стека главного потока задаёт PE-заголовок exe, а не код.
# Движок на коннекте вызывает метод CNetChan с кадром 0x100138 (1 048 888 байт),
# что БОЛЬШЕ дефолтного резерва в 1 МБ (1 048 576) на 312 байт — отсюда
# STATUS_STACK_OVERFLOW, который движок глушит и убивает процесс кодом 100.
#
# Но задирать резерв нельзя: SizeOfStackReserve наследуется КАЖДЫМ потоком,
# созданным с нулевым размером стека. При 73 потоках 8 МБ = 584 МБ виртуального
# адресного пространства, и игра падает с "OUT OF MEMORY" при загрузке карты.
# 3 МБ хватает с запасом, коммит маленький (стек дорастёт по мере надобности).
#
# /LARGEADDRESSAWARE обязателен: без него 32-битный процесс зажат в 2 ГБ, а
# внутри Chromium (panorama) + движок. У родного csgo.exe от Valve он есть.
if(MSVC)
    target_link_options(csgo_launcher PRIVATE
        "/STACK:3145728,131072"
        "/LARGEADDRESSAWARE"
    )
endif()