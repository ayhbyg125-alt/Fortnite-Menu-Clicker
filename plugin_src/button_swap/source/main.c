// Custom Screen Cover + Configurable Auto-Clicker Menu for GoldHEN
#include "plugin_common.h"
#include "Common.h"
#include <orbis/Pad.h>
#include <orbis/libkernel.h>

attr_public const char *g_pluginName = "fn_hide_menu";
attr_public const char *g_pluginDesc = "Blackout screen + configurable button/interval auto-clicker";
attr_public const char *g_pluginAuth = "Custom";
attr_public u32 g_pluginVersion = 0x00000200;

s32 (*scePadReadState)(s32 handle, OrbisPadData *pData);
s32 (*sceVideoOutSubmitFlip)(s32 handle, s32 bufferIndex, s32 flipMode, s64 flipArg);

HOOK_INIT(scePadReadState);
HOOK_INIT(sceVideoOutSubmitFlip);

static bool g_BlackoutActive = true;
static bool g_AutoClickActive = true;
static bool g_MenuMode = false;

typedef struct {
    const char *name;
    u32 mask;
} ButtonOption;

static const ButtonOption g_Buttons[] = {
    { "Cross (X)",      ORBIS_PAD_BUTTON_CROSS },
    { "Circle (O)",     ORBIS_PAD_BUTTON_CIRCLE },
    { "Square ([])",    ORBIS_PAD_BUTTON_SQUARE },
    { "Triangle (/\\)", ORBIS_PAD_BUTTON_TRIANGLE },
    { "L1",             ORBIS_PAD_BUTTON_L1 },
    { "R1",             ORBIS_PAD_BUTTON_R1 },
    { "L2",             ORBIS_PAD_BUTTON_L2 },
    { "R2",             ORBIS_PAD_BUTTON_R2 },
    { "L3",             ORBIS_PAD_BUTTON_L3 },
    { "R3",             ORBIS_PAD_BUTTON_R3 },
    { "D-Pad Up",       ORBIS_PAD_BUTTON_UP },
    { "D-Pad Down",     ORBIS_PAD_BUTTON_DOWN },
    { "D-Pad Left",     ORBIS_PAD_BUTTON_LEFT },
    { "D-Pad Right",    ORBIS_PAD_BUTTON_RIGHT },
    { "Options",        ORBIS_PAD_BUTTON_OPTIONS }
};

#define NUM_BUTTONS ((s32)(sizeof(g_Buttons) / sizeof(g_Buttons[0])))

static s32 g_BtnIdx = 0;
static s32 g_UnitMs = 0;
static s32 g_DelaySecTenths = 20;
static s32 g_DelayMs = 500;

static u32 g_PrevButtons = 0;
static u64 g_LastClickTime = 0;

static void ShowStatusToast(const char *header) {
    char msg[256];
    if (g_UnitMs == 0) {
        snprintf(msg, sizeof(msg), "%s\nHide: %s | Btn: %s | %d.%ds\n(L1+Down: Hide | L1+R1: Menu)",
                 header,
                 g_BlackoutActive ? "ON" : "OFF",
                 g_Buttons[g_BtnIdx].name,
                 g_DelaySecTenths / 10,
                 g_DelaySecTenths % 10);
    } else {
        snprintf(msg, sizeof(msg), "%s\nHide: %s | Btn: %s | %dms\n(L1+Down: Hide | L1+R1: Menu)",
                 header,
                 g_BlackoutActive ? "ON" : "OFF",
                 g_Buttons[g_BtnIdx].name,
                 g_DelayMs);
    }
    Notify(TEX_ICON_SYSTEM, msg);
}

s32 sceVideoOutSubmitFlip_hook(s32 handle, s32 bufferIndex, s32 flipMode, s64 flipArg) {
    if (g_BlackoutActive) {
        return HOOK_CONTINUE(sceVideoOutSubmitFlip, s32(*)(s32, s32, s32, s64), handle, -1, flipMode, flipArg);
    }
    return HOOK_CONTINUE(sceVideoOutSubmitFlip, s32(*)(s32, s32, s32, s64), handle, bufferIndex, flipMode, flipArg);
}

s32 scePadReadState_hook(s32 handle, OrbisPadData *pData) {
    s32 ret = HOOK_CONTINUE(scePadReadState, s32(*)(s32, OrbisPadData *), handle, pData);
    if (ret != 0 || pData == NULL) {
        return ret;
    }

    u32 cur = pData->buttons;
    u32 newlyPressed = cur & ~g_PrevButtons;
    g_PrevButtons = cur;

    u32 hideCombo = ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_DOWN;
    if ((cur & hideCombo) == hideCombo && (newlyPressed & hideCombo)) {
        g_BlackoutActive = !g_BlackoutActive;
        g_AutoClickActive = g_BlackoutActive;
        g_LastClickTime = sceKernelGetProcessTime();
        ShowStatusToast(g_BlackoutActive ? "Screen Hidden + Macro ON" : "Screen Visible + Macro OFF");
    }

    u32 menuCombo = ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1;
    if ((cur & menuCombo) == menuCombo && (newlyPressed & menuCombo)) {
        g_MenuMode = !g_MenuMode;
        ShowStatusToast(g_MenuMode ? "MENU OPEN (D-Pad/Square)" : "MENU CLOSED");
    }

    if (g_MenuMode) {
        if (newlyPressed & ORBIS_PAD_BUTTON_RIGHT) {
            g_BtnIdx = (g_BtnIdx + 1) % NUM_BUTTONS;
            ShowStatusToast("Changed Target Button");
        } else if (newlyPressed & ORBIS_PAD_BUTTON_LEFT) {
            g_BtnIdx = (g_BtnIdx - 1 + NUM_BUTTONS) % NUM_BUTTONS;
            ShowStatusToast("Changed Target Button");
        } else if (newlyPressed & ORBIS_PAD_BUTTON_SQUARE) {
            g_UnitMs = !g_UnitMs;
            ShowStatusToast(g_UnitMs ? "Switched to Milliseconds (ms)" : "Switched to Seconds (s)");
        } else if (newlyPressed & ORBIS_PAD_BUTTON_UP) {
            if (g_UnitMs == 0) {
                if (g_DelaySecTenths < 100) g_DelaySecTenths += 5;
            } else {
                if (g_DelayMs < 5000) g_DelayMs += 50;
            }
            ShowStatusToast("Increased Delay");
        } else if (newlyPressed & ORBIS_PAD_BUTTON_DOWN) {
            if (g_UnitMs == 0) {
                if (g_DelaySecTenths > 5) g_DelaySecTenths -= 5;
            } else {
                if (g_DelayMs > 50) g_DelayMs -= 50;
            }
            ShowStatusToast("Decreased Delay");
        }
        pData->buttons &= ~(ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN |
                            ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT |
                            ORBIS_PAD_BUTTON_SQUARE);
    }

    if (g_AutoClickActive && !g_MenuMode) {
        u64 intervalMicros = (g_UnitMs == 0)
            ? ((u64)g_DelaySecTenths * 100000ULL)
            : ((u64)g_DelayMs * 1000ULL);

        u64 now = sceKernelGetProcessTime();
        u64 elapsed = now - g_LastClickTime;

        if (elapsed >= intervalMicros) {
            g_LastClickTime = now;
        } else if (elapsed < 60000ULL) {
            pData->buttons |= g_Buttons[g_BtnIdx].mask;
        }
    }

    return ret;
}

s32 attr_public plugin_load(s32 argc, const char* argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    boot_ver();

    s32 padHandle = 0;
    sys_dynlib_load_prx("libScePad.sprx", &padHandle);
    sys_dynlib_dlsym(padHandle, "scePadReadState", &scePadReadState);
    HOOK32(scePadReadState);

    s32 vidHandle = 0;
    sys_dynlib_load_prx("libSceVideoOut.sprx", &vidHandle);
    sys_dynlib_dlsym(vidHandle, "sceVideoOutSubmitFlip", &sceVideoOutSubmitFlip);
    HOOK32(sceVideoOutSubmitFlip);

    ShowStatusToast("FN Cover Loaded (Active)");
    return 0;
}

s32 attr_public plugin_unload(s32 argc, const char* argv[]) {
    UNHOOK(scePadReadState);
    UNHOOK(sceVideoOutSubmitFlip);
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void *args) {
    return 0;
}

s32 attr_module_hidden module_stop(s64 argc, const void *args) {
    return 0;
}
