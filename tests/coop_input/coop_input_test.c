/* Exercise the real SDL input boundary, including hotplug and identical pads. */
#include "controller.h"
#include "config.h"
#include "keybinds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef COOP_TEST_UI
#include "consoles/nes/nes_binds.h"
#endif

#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s (%s)\n",__LINE__,#x,SDL_GetError());exit(1); } } while(0)
static SDL_Joystick *pads[4];
static SDL_JoystickID ids[4];
static int attach(int n) {
    int device=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        SDL_CONTROLLER_AXIS_MAX,SDL_CONTROLLER_BUTTON_MAX,0);
    CHECK(device>=0);CHECK(SDL_IsGameController(device));
    pads[n]=SDL_JoystickOpen(device);CHECK(pads[n]);
    ids[n]=SDL_JoystickInstanceID(pads[n]);
    return device;
}
static void button(int n,int b,int pressed) {
    CHECK(SDL_JoystickSetVirtualButton(pads[n],b,(Uint8)pressed)==0);
    SDL_JoystickUpdate();SDL_GameControllerUpdate();
}
static void hotplug(Uint32 type,int which) {
    SDL_Event e;memset(&e,0,sizeof e);e.type=type;e.cdevice.which=which;
    controller_handle_event(&e);
}
int main(void) {
    SDL_SetMainReady();CHECK(SDL_Init(SDL_INIT_GAMECONTROLLER)==0);
    /* CTest's private build directory contains all generated test settings. */
    FILE *f=fopen("keybinds.ini","w");CHECK(f);
    fputs("[player1]\na=Z\n[player2]\na=F\n[player3]\na=J\n[player4]\na=Keypad 0\n"
          "[gamepad3]\nb=leftshoulder\n[gamepad4]\na=rightshoulder\n",f);fclose(f);
    keybinds_init(NULL);
    CHECK(keybinds_get()->extra[0].start==SDL_SCANCODE_BACKSLASH);
    Uint8 keys[SDL_NUM_SCANCODES]={0};keys[SDL_SCANCODE_J]=1;
    CHECK(keybinds_read_player(keys,3)==0x80);
    CHECK(keybinds_read_player(keys,1)==0 && keybinds_read_player(keys,2)==0 && keybinds_read_player(keys,4)==0);
    config_set_defaults(&g_nes_config);
    CHECK(g_nes_config.player_src[2]==2 && g_nes_config.deadzone[3]==30);
    g_nes_config.player_src[3]=1;g_nes_config.deadzone[2]=45;
    strcpy(g_nes_config.player_gamepad_guid[1],"test-guid");
    config_save("input-test.ini");config_load("input-test.ini");
    CHECK(g_nes_config.player_src[3]==1 && g_nes_config.deadzone[2]==45);
    CHECK(!strcmp(g_nes_config.player_gamepad_guid[1],"test-guid"));
    config_set_defaults(&g_nes_config);
    int initial=SDL_NumJoysticks();
    CHECK(initial==0); /* Run without physical pads: first-pad auto assignment. */
    for(int n=0;n<3;++n) attach(n);
    controller_init();CHECK(controller_count()==3);
    button(0,SDL_CONTROLLER_BUTTON_A,1);
    CHECK(controller_read_player(1)==0 && controller_read_player(2)==0x80);
    CHECK(controller_read_player(3)==0 && controller_read_player(4)==0);
    CHECK(SDL_JoystickSetVirtualAxis(pads[0],SDL_CONTROLLER_AXIS_LEFTX,12000)==0);
    SDL_JoystickUpdate();SDL_GameControllerUpdate();
    CHECK(controller_read_player(2)==0x81);
    g_nes_config.deadzone[1]=50;CHECK(controller_read_player(2)==0x80);
    CHECK(SDL_JoystickSetVirtualAxis(pads[0],SDL_CONTROLLER_AXIS_LEFTX,0)==0);
    SDL_JoystickUpdate();SDL_GameControllerUpdate();
    button(1,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,1);
    CHECK(controller_read_player(3)==0x40);
    CHECK(controller_instance_is_player(ids[2],4));
    CHECK(SDL_JoystickDetachVirtual(0)==0);hotplug(SDL_CONTROLLERDEVICEREMOVED,ids[0]);
    CHECK(controller_read_player(2)==0);
    CHECK(controller_instance_is_player(ids[1],3) && controller_instance_is_player(ids[2],4));
    SDL_JoystickClose(pads[0]);
    int replacement=attach(0);hotplug(SDL_CONTROLLERDEVICEADDED,replacement);
    button(0,SDL_CONTROLLER_BUTTON_A,1);CHECK(controller_read_player(2)==0x80);
    controller_shutdown();
    attach(3);
    for(int n=0;n<4;++n) {
        g_nes_config.player_src[n]=2;
        g_nes_config.player_gamepad_instance[n]=ids[3-n];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(pads[n]),g_nes_config.player_gamepad_guid[n],40);
    }
    controller_init();CHECK(controller_count()==4);
    for(int n=0;n<4;++n) CHECK(controller_instance_is_player(ids[3-n],n+1));
    button(0,SDL_CONTROLLER_BUTTON_A,0);button(0,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,1);
    CHECK(controller_read_player(4)==0x80); /* P4's binding, selected physical pad. */
    controller_shutdown();
    for(int n=0;n<4;++n) SDL_JoystickClose(pads[n]);
    while(SDL_NumJoysticks()) CHECK(SDL_JoystickDetachVirtual(0)==0);
#ifdef COOP_TEST_UI
    rui_nes_binds_init("keybinds.ini");
    rui_nes_binds_set("keybinds.ini",2,5,SDL_SCANCODE_V);
    rui_nes_binds_set("keybinds.ini",3,4,SDL_SCANCODE_U);
    rui_nes_binds_init("keybinds.ini");
    CHECK(rui_nes_binds_get("keybinds.ini",2,5)==SDL_SCANCODE_V);
    CHECK(rui_nes_binds_get("keybinds.ini",3,4)==SDL_SCANCODE_U);
    keybinds_init(NULL);
    memset(keys,0,sizeof keys);keys[SDL_SCANCODE_V]=keys[SDL_SCANCODE_U]=1;
    CHECK(keybinds_read_player(keys,3)==0x40 && keybinds_read_player(keys,4)==0x80);
    CHECK(keybinds_get_pad(4)->btn_mask[0]==(1u<<SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    rui_nes_binds_reset("keybinds.ini",3);keybinds_init(NULL);
    CHECK(keybinds_read_player(keys,4)==0 && keybinds_read_player(keys,3)==0x40);
    keys[SDL_SCANCODE_UNKNOWN]=1;CHECK(keybinds_read_player(keys,4)==0);
#endif
    SDL_Quit();puts("PASS: keyboard seats, config persistence, pad assignment, hotplug, four identical pads, UI bindings");
    return 0;
}
