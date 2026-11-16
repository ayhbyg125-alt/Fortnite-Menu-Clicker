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

/* ------------------------------------------------------------------ */
/* Button masks (ORBIS_PAD_BUTTON_*)                                   */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
#define HOLD_US        60000ull   /* simulated press length: 60 ms */
#define MIN_INTERVAL_MS 100u
#define MAX_INTERVAL_MS 600000u

static volatile bool g_enabled = true;    /* blackout + autoclick, ON at boot */
static volatile bool g_menu    = false;
static volatile bool g_capture = false;
static int      g_sel        = 0;         /* 0 target, 1 unit, 2 delay */
static bool     g_unitMs     = false;     /* false = seconds */
static uint32_t g_intervalMs = 2000;
static uint32_t g_target     = BTN_CROSS;

static uint32_t g_prev       = 0;         /* previous physical buttons */
static uint64_t g_nextClick  = 0;
static uint64_t g_pressUntil = 0;

/* ------------------------------------------------------------------ */
/* Notifications                                                       */
/* ------------------------------------------------------------------ */
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
        snprintf(buf, sizeof(buf),
                 "== AUTO CLICKER ==\nPRESS ANY BUTTON TO BIND...");
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

/* ------------------------------------------------------------------ */
/* Input logic                                                         */
/* ------------------------------------------------------------------ */
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
    if (!g_unitMs) {                       /* back to seconds: round to whole s */
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

    /* L1 + R1 : open/close menu (also cancels capture) */
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

    /* Capture mode: next new press becomes the target */
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

    /* L1 + D-Pad Down : global toggle (blackout + autoclick) */
    if (l1 && (pressed & BTN_DOWN)) {
        g_enabled = !g_enabled;
        g_nextClick = now + g_intervalMs * 1000ull;
        g_pressUntil = 0;
        notify(g_enabled ? "Auto clicker: ON (blackout)" : "Auto clicker: OFF");
    }
}

static uint32_t filter_buttons(uint32_t phys, uint64_t now) {
    if (g_menu || g_capture) return 0;     /* menu eats all game input */
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

/* ------------------------------------------------------------------ */
/* Pad hooks                                                           */
/* ------------------------------------------------------------------ */
static Detour g_dReadState, g_dRead, g_dReg, g_dFlip;
static int32_t (*scePadReadState_stub)(int32_t, OrbisPadData *);
static int32_t (*scePadRead_stub)(int32_t, OrbisPadData *, int32_t);
static int32_t (*RegisterBuffers_stub)(int32_t, int32_t, void *const *, int32_t,
                                       const OrbisVideoOutBufferAttribute *);
static int32_t (*SubmitFlip_stub)(int32_t, int32_t, uint32_t, int64_t);

static int32_t scePadReadState_hook(int32_t handle, OrbisPadData *data) {
    int32_t ret = scePadReadState_stub(handle, data);
    if (ret == 0 && data) {
        uint64_t now = sceKernelGetProcessTime();
        uint32_t phys = data->buttons;
        handle_input(phys, now);
        data->buttons = filter_buttons(phys, now);
    }
    return ret;
}

static int32_t scePadRead_hook(int32_t handle, OrbisPadData *data, int32_t count) {
    int32_t ret = scePadRead_stub(handle, data, count);
    if (ret > 0 && data) {
        uint64_t now = sceKernelGetProcessTime();
        handle_input(data[ret - 1].buttons, now);        /* newest sample */
        for (int32_t i = 0; i < ret; i++)
            data[i].buttons = filter_buttons(data[i].buttons, now);
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* Blackout: zero the framebuffer right before every flip              */
/* ------------------------------------------------------------------ */
#define MAX_FB 16
static void    *g_fbAddr[MAX_FB];
static size_t   g_fbSize = 0;

static int32_t RegisterBuffers_hook(int32_t handle, int32_t startIndex,
                                    void *const *addresses, int32_t bufferNum,
                                    const OrbisVideoOutBufferAttribute *attr) {
    int32_t ret = RegisterBuffers_stub(handle, startIndex, addresses, bufferNum, attr);
    if (ret >= 0 && addresses && attr && attr->width > 0 && attr->height > 0) {
        size_t w = ((size_t)attr->width  + 31u) & ~(size_t)31u;   /* tile aligned */
        size_t h = ((size_t)attr->height + 31u) & ~(size_t)31u;
        g_fbSize = w * h * 4u;
        for (int32_t i = 0; i < bufferNum; i++) {
            int32_t idx = startIndex + i;
            if (idx >= 0 && idx < MAX_FB) g_fbAddr[idx] = addresses[i];
        }
    }
    return ret;
}

static int32_t SubmitFlip_hook(int32_t handle, int32_t bufferIndex,
                               uint32_t flipMode, int64_t flipArg) {
    if (g_enabled && !g_menu && !g_capture &&
        bufferIndex >= 0 && bufferIndex < MAX_FB &&
        g_fbAddr[bufferIndex] && g_fbSize) {
        memset(g_fbAddr[bufferIndex], 0, g_fbSize);
    }
    return SubmitFlip_stub(handle, bufferIndex, flipMode, flipArg);
}

/* ------------------------------------------------------------------ */
/* Plugin entry points                                                 */
/* ------------------------------------------------------------------ */
static void *install(Detour *d, void *target, void *hook) {
    Detour_Construct(d, false);
    return (void *)Detour_DetourFunction(d, (uint64_t)target, hook);
}

attr_public int32_t plugin_load(int32_t argc, const char *argv[]) {
    (void)argc; (void)argv;

    scePadReadState_stub = install(&g_dReadState, (void *)scePadReadState, (void *)scePadReadState_hook);
    scePadRead_stub      = install(&g_dRead,      (void *)scePadRead,      (void *)scePadRead_hook);
    RegisterBuffers_stub = install(&g_dReg,       (void *)sceVideoOutRegisterBuffers, (void *)RegisterBuffers_hook);
    SubmitFlip_stub      = install(&g_dFlip,      (void *)sceVideoOutSubmitFlip,      (void *)SubmitFlip_hook);

    g_nextClick = 0;
    notify("Fortnite Menu Clicker loaded: ON\nL1+Down toggle | L1+R1 menu");
    return 0;
}

attr_public int32_t plugin_unload(int32_t argc, const char *argv[]) {
    (void)argc; (void)argv;
    g_enabled = false;
    g_menu = false;
    g_capture = false;
    return 0;
}

attr_public int module_start(size_t argc, const void *args) {
    (void)argc; (void)args;
    return 0;
}

attr_public int module_stop(size_t argc, const void *args) {
    (void)argc; (void)args;
    return 0;
}
