// plugin_src/button_swap/source/main.c
// Fortnite Menu Clicker - blackout + auto-click GoldHEN plugin (pure C)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/VideoOut.h>
#include <GoldHEN.h>
#include <Detour.h>

#define attr_public __attribute__((visibility("default")))

attr_public const char *g_pluginName = "button_swap";
attr_public const char *g_pluginDesc = "Fullscreen blackout + auto-clicker with OSD menu";
attr_public const char *g_pluginAuth = "Fortnite-Menu-Clicker";
attr_public uint32_t g_pluginVersion = 0x00000100;

#define BTN_L3        0x00000002u
#define BTN_R3        0x00000004u
#define BTN_OPTIONS   0x00000008u
#define BTN_UP        0x00000010u
#define BTN_RIGHT     0x00000020u
#define BTN_DOWN      0x00000040u
#define BTN_LEFT      0x00000080u
#define BTN_L2        0x00000100u
#define BTN_R2        0x00000200u
#define BTN_L1        0x00000400u
#define BTN_R1        0x00000800u
#define BTN_TRIANGLE  0x00001000u
#define BTN_CIRCLE    0x00002000u
#define BTN_CROSS     0x00004000u
#define BTN_SQUARE    0x00008000u
#define BTN_TOUCHPAD  0x00100000u

typedef struct { uint32_t mask; const char *name; } BtnName;
static const BtnName g_btnNames[] = {
    { BTN_CROSS,    "Cross (X)"    }, { BTN_CIRCLE,  "Circle"   },
    { BTN_SQUARE,   "Square"       }, { BTN_TRIANGLE,"Triangle" },
    { BTN_L1,       "L1"           }, { BTN_R1,      "R1"       },
    { BTN_L2,       "L2"           }, { BTN_R2,      "R2"       },
    { BTN_L3,       "L3"           }, { BTN_R3,      "R3"       },
    { BTN_UP,       "D-Pad Up"     }, { BTN_DOWN,    "D-Pad Down"  },
    { BTN_LEFT,     "D-Pad Left"   }, { BTN_RIGHT,   "D-Pad Right" },
    { BTN_OPTIONS,  "Options"      }, { BTN_TOUCHPAD,"Touchpad" },
};
#define BTN_COUNT (sizeof(g_btnNames) / sizeof(g_btnNames[0]))

static const char *btn_name(uint32_t mask) {
    for (size_t i = 0; i < BTN_COUNT; i++)
        if (g_btnNames[i].mask == mask) return g_btnNames[i].name;
    return "Unknown";
}

#define HOLD_US         60000ull
#define MIN_INTERVAL_MS 100u
#define MAX_INTERVAL_MS 600000u

static volatile bool g_enabled = true;
static volatile bool g_menu    = false;
static volatile bool g_capture = false;
static int      g_sel        = 0;
static bool     g_unitMs     = false;
static uint32_t g_intervalMs = 2000;
static uint32_t g_target     = BTN_CROSS;

static uint32_t g_prev       = 0;
static uint64_t g_nextClick  = 0;
static uint64_t g_pressUntil = 0;

static void notify(const char *msg) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = 0;
    req.targetId = -1;
    req.useIconImageUri = 0;
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void show_menu(void) {
    char buf[320];
    if (g_capture) {
        snprintf(buf, sizeof(buf), "== AUTO CLICKER ==\nPRESS ANY BUTTON TO BIND...");
        notify(buf);
        return;
    }
    char delay[32];
    if (g_unitMs) snprintf(delay, sizeof(delay), "%u ms", (unsigned)g_intervalMs);
    else          snprintf(delay, sizeof(delay), "%u s",  (unsigned)(g_intervalMs / 1000));

    snprintf(buf, sizeof(buf),
             "== AUTO CLICKER MENU ==\n%s Target: %s  [X to rebind]\n%s Unit: %s\n%s Delay: %s  [Left/Right]\n(Up/Down select, L1+R1 close)",
             g_sel == 0 ? ">" : " ", btn_name(g_target),
             g_sel == 1 ? ">" : " ", g_unitMs ? "ms" : "s",
             g_sel == 2 ? ">" : " ", delay);
    notify(buf);
}
static void adjust_delay(int dir) {
    if (g_unitMs) {
        int64_t v = (int64_t)g_intervalMs + (int64_t)dir * 50;
        if (v < MIN_INTERVAL_MS) v = MIN_INTERVAL_MS;
        if (v > MAX_INTERVAL_MS) v = MAX_INTERVAL_MS;
        g_intervalMs = (uint32_t)v;
    } else {
        int64_t s = (int64_t)(g_intervalMs / 1000) + dir;
        if (s < 1) s = 1;
        if (s > 600) s = 600;
        g_intervalMs = (uint32_t)s * 1000u;
    }
}

static void toggle_unit(void) {
    g_unitMs = !g_unitMs;
    if (!g_unitMs) {
        uint32_t s = (g_intervalMs + 500) / 1000;
        if (s < 1) s = 1;
        g_intervalMs = s * 1000u;
    }
}

static void handle_input(uint32_t phys, uint64_t now) {
    uint32_t pressed = phys & ~g_prev;
    g_prev = phys;

    bool l1 = (phys & BTN_L1) != 0;
    bool r1 = (phys & BTN_R1) != 0;

    if (l1 && r1 && (pressed & (BTN_L1 | BTN_R1))) {
        if (g_capture) g_capture = false;
        g_menu = !g_menu;
        if (g_menu) show_menu();
        else {
            g_nextClick = now + g_intervalMs * 1000ull;
            notify(g_enabled ? "Auto clicker: ON (blackout)" : "Auto clicker: OFF");
        }
        return;
    }

    if (g_menu && g_capture) {
        if (pressed) {
            for (size_t i = 0; i < BTN_COUNT; i++) {
                if (pressed & g_btnNames[i].mask) {
                    g_target = g_btnNames[i].mask;
                    g_capture = false;
                    show_menu();
                    break;
                }
            }
        }
        return;
    }

    if (g_menu) {
        bool changed = false;
        if (pressed & BTN_UP)   { g_sel = (g_sel + 2) % 3; changed = true; }
        if (pressed & BTN_DOWN) { g_sel = (g_sel + 1) % 3; changed = true; }
        if (pressed & BTN_CROSS) {
            if (g_sel == 0)      { g_capture = true; changed = true; }
            else if (g_sel == 1) { toggle_unit();    changed = true; }
        }
        if (pressed & BTN_LEFT) {
            if (g_sel == 2)      { adjust_delay(-1); changed = true; }
            else if (g_sel == 1) { toggle_unit();    changed = true; }
        }
        if (pressed & BTN_RIGHT) {
            if (g_sel == 2)      { adjust_delay(+1); changed = true; }
            else if (g_sel == 1) { toggle_unit();    changed = true; }
        }
        if (changed) show_menu();
        return;
    }

    if (l1 && (pressed & BTN_DOWN)) {
        g_enabled = !g_enabled;
        g_nextClick = now + g_intervalMs * 1000ull;
        g_pressUntil = 0;
        notify(g_enabled ? "Auto clicker: ON (blackout)" : "Auto clicker: OFF");
    }
}

static uint32_t filter_buttons(uint32_t phys, uint64_t now) {
    if (g_menu || g_capture) return 0;
    uint32_t out = phys;
    if (g_enabled) {
        if (now >= g_nextClick) {
            g_pressUntil = now + HOLD_US;
            g_nextClick  = now + (uint64_t)g_intervalMs * 1000ull;
        }
        if (now < g_pressUntil) out |= g_target;
    }
    return out;
}

static Detour g_dReadState;
static Detour g_dRead;
static Detour g_dSubmitFlip;

static int hk_scePadReadState(int handle, OrbisPadData *data) {
    int ret = Detour_Call(&g_dReadState, int (*)(int, OrbisPadData *), handle, data);
    if (ret == 0 && data && data->connected) {
        uint64_t now = sceKernelGetProcessTime();
        handle_input(data->buttons, now);
        data->buttons = filter_buttons(data->buttons, now);
    }
    return ret;
}

static int hk_scePadRead(int handle, OrbisPadData *data, int num) {
    int count = Detour_Call(&g_dRead, int (*)(int, OrbisPadData *, int), handle, data, num);
    if (count > 0 && data) {
        uint64_t now = sceKernelGetProcessTime();
        for (int i = 0; i < count; i++) {
            if (data[i].connected) {
                handle_input(data[i].buttons, now);
                data[i].buttons = filter_buttons(data[i].buttons, now);
            }
        }
    }
    return count;
}

static int hk_sceVideoOutSubmitFlip(int handle, int index, unsigned int flipMode, int64_t flipArg) {
    if (g_enabled) {
        return Detour_Call(&g_dSubmitFlip, int (*)(int, int, unsigned int, int64_t), handle, -1, flipMode, flipArg);
    }
    return Detour_Call(&g_dSubmitFlip, int (*)(int, int, unsigned int, int64_t), handle, index, flipMode, flipArg);
}

attr_public int module_start(size_t argc, const void *args) {
    g_nextClick = sceKernelGetProcessTime() + (uint64_t)g_intervalMs * 1000ull;

    Detour_Construct(&g_dReadState, DetourMode_x64);
    Detour_DetourFunction(&g_dReadState, (void *)scePadReadState, (void *)hk_scePadReadState);

    Detour_Construct(&g_dRead, DetourMode_x64);
    Detour_DetourFunction(&g_dRead, (void *)scePadRead, (void *)hk_scePadRead);

    Detour_Construct(&g_dSubmitFlip, DetourMode_x64);
    Detour_DetourFunction(&g_dSubmitFlip, (void *)sceVideoOutSubmitFlip, (void *)hk_sceVideoOutSubmitFlip);

    notify("FN Blackout + AutoClicker: ON\nL1+Down: Toggle | L1+R1: Menu");
    return 0;
}

attr_public int module_stop(size_t argc, const void *args) {
    Detour_Destroy(&g_dReadState);
    Detour_Destroy(&g_dRead);
    Detour_Destroy(&g_dSubmitFlip);
    return 0;
}
