cmake_minimum_required(VERSION 3.20)
project(NESRecompCoopInput C)
set(CMAKE_C_STANDARD 11)
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../runner/external/SDL2/cmake")
find_package(SDL2 REQUIRED)
add_executable(coop_input_test coop_input_test.c
    ../../runner/src/controller.c ../../runner/src/config.c ../../runner/src/keybinds.c)
target_include_directories(coop_input_test PRIVATE ../../runner/include ${SDL2_INCLUDE_DIRS})
target_compile_definitions(coop_input_test PRIVATE NESRECOMP_INPUT_SEATS=4 SDL_MAIN_HANDLED)
target_link_libraries(coop_input_test PRIVATE ${SDL2_LIBRARIES})
set(RECOMP_UI_ROOT "" CACHE PATH "Optional UI checkout to test keyboard persistence into the runner")
if(RECOMP_UI_ROOT)
    target_sources(coop_input_test PRIVATE
        "${RECOMP_UI_ROOT}/src/consoles/nes/nes_binds.c")
    if(EXISTS "${RECOMP_UI_ROOT}/src/common/launcher_ini.inc")
        # Current UI exposes the same production INI writer independently.
        target_compile_definitions(coop_input_test PRIVATE COOP_TEST_UI_SHARED_INI)
    else()
        # Compatibility with UI pins before the persistence helper was split.
        target_sources(coop_input_test PRIVATE
            "${RECOMP_UI_ROOT}/src/common/launcher_binds.c"
            "${RECOMP_UI_ROOT}/src/common/keybinds.c"
            "${RECOMP_UI_ROOT}/src/consoles/psx/psx_binds.c"
            "${RECOMP_UI_ROOT}/src/consoles/n64/n64_binds.c"
            "${RECOMP_UI_ROOT}/src/consoles/genesis/genesis_binds.c"
            "${RECOMP_UI_ROOT}/src/consoles/gb/gb_binds.c")
    endif()
    target_include_directories(coop_input_test PRIVATE "${RECOMP_UI_ROOT}/src" "${RECOMP_UI_ROOT}/src/common")
    target_compile_definitions(coop_input_test PRIVATE COOP_TEST_UI SNESRECOMP_SDL2)
endif()
if(WIN32)
    add_custom_command(TARGET coop_input_test POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../../runner/external/SDL2/lib/x64/SDL2.dll" "$<TARGET_FILE_DIR:coop_input_test>")
endif()
enable_testing()
add_test(NAME coop_input COMMAND coop_input_test)

if(WIN32)
    # Exercise virtual pads without depending on the owner's connected devices.
    set_tests_properties(coop_input PROPERTIES ENVIRONMENT
        "SDL_JOYSTICK_HIDAPI=0;SDL_JOYSTICK_WGI=0;SDL_XINPUT_ENABLED=0;SDL_DIRECTINPUT_ENABLED=0;SDL_JOYSTICK_RAWINPUT=0")
endif()