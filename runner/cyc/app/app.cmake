# runner/cyc/app/app.cmake - application layer for a cycle-backend game (launcher, in-game menu, mods, save states).
#
#   include("${NESRECOMP_ROOT}/runner/cyc/project.cmake")
#   include("${NESRECOMP_ROOT}/runner/cyc/app/app.cmake")
#   nesrecomp_add_cycle_game(MyGame ROM ... HEADLESS)     # HEADLESS: cyc_sdl.c is replaced by cyc_app.c
#   nesrecomp_cyc_app(MyGame)
#   <recomp-ui>: recomp_target_launcher_ui(MyGame CONSOLE nes) / recomp_target_runtime_ui_sdlrenderer2(MyGame)
#
# The game supplies cyc_app_game() (cyc_app.h). The project must enable C and CXX.
include_guard(GLOBAL)

function(nesrecomp_cyc_app target)
    get_filename_component(cyc_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    get_filename_component(runner_dir "${cyc_dir}/.." ABSOLUTE)

    # The interactive host replaces cyc_sdl.c; the generic host stays, with its main() renamed so cyc_app.c can run
    # the launcher first and reuse its ROM checks, headless options and miss log.
    set_source_files_properties("${cyc_dir}/cyc_host.c" PROPERTIES COMPILE_DEFINITIONS "main=cyc_host_main")
    target_sources(${target} PRIVATE
        "${cyc_dir}/app/cyc_app.c"
        "${runner_dir}/src/config.c"
        "${runner_dir}/src/crc32.c"
        "${runner_dir}/src/keybinds.c"
        "${runner_dir}/src/controller.c"
        "${runner_dir}/src/mod_runtime.cpp"
        "${runner_dir}/src/runtime_ui_host.cpp")
    target_include_directories(${target} PRIVATE "${runner_dir}/include" "${cyc_dir}/app")
    target_compile_definitions(${target} PRIVATE CYC_WITH_SDL CYC_APP_HOOKS NESRECOMP_ENABLE_MODS=1 NESRECOMP_RUNTIME_UI=1)
    target_compile_features(${target} PRIVATE cxx_std_17)

    list(APPEND CMAKE_PREFIX_PATH "${runner_dir}/external/SDL2/cmake")
    find_package(SDL2 REQUIRED CONFIG)
    target_link_libraries(${target} PRIVATE SDL2::SDL2)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:${target}>)
    endif()
endfunction()
