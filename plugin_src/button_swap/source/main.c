#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <orbis/Pad.h>
#include <orbis/VideoOut.h>
#include "Detour.h"   // verify signatures against your SDK version

#define COMBO_MENU    (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_DOWN)
#define COMBO_TOGGLE  (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1)

static volatile bool g_enabled = false;
static uint32_t g_prevButtons = 0;

static Detour g_dReadState, g_dRead, g_dFlip;

typedef int (*scePadReadState_t)(int32_t, OrbisPadData *);
typedef int (*scePadRead_t)(int32_t, OrbisPadData *, int32_t);
typedef int (*sceVideoOutSubmitFlip_t)(int32_t, int32_t, uint32_t, int64_t);

static scePadReadState_t       orig_scePadReadState;
static scePadRead_t            orig_scePadRead;
static sceVideoOutSubmitFlip_t orig_sceVideoOutSubmitFlip;

static void show_menu(void) {
    char buf[320];
    char delay[32];
    snprintf(delay, sizeof(delay), "%d ms", 0);
    snprintf(buf, sizeof(buf), "Blackout: %s\nDelay: %s",
             g_enabled ? "ON" : "OFF", delay);
    // display buf (e.g., notification), but not every frame
}

static void process_buttons(uint32_t buttons) {
    uint32_t pressed = buttons & ~g_prevButtons;   // rising edges only
    g_prevButtons = buttons;

    if ((buttons & COMBO_TOGGLE) == COMBO_TOGGLE && (pressed & COMBO_TOGGLE))
        g_enabled = !g_enabled;
    if ((buttons & COMBO_MENU) == COMBO_MENU && (pressed & COMBO_M
