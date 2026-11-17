// plugin_src/button_swap/source/main.c
// Fortnite Menu Hider - fullscreen framebuffer blackout + startup auto-click
// Local GoldHEN plugin. Blackout is controlled independently from input.
//
// Important: Detour_DetourFunction() returns the original trampoline.
// Do not use Detour_Call().

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

/* libc declaration used to query this PRX's own process ID. */
extern pid_t getpid(void);

#define attr_public __attribute__((visibility("default")))

attr_public const char *g_pluginName = "button_swap";
attr_public const char *g_pluginDesc = "Fullscreen blackout + startup auto-click";
attr_public const char *g_pluginAuth = "Fortnite-Menu-Hider";
attr_public uint32_t g_pluginVersion = 0x00000200;

/* PS4 pad masks. */
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

#define MIN_INTERVAL_MS 100u
#define MAX_INTERVAL_MS 600000u
#define MIN_HOLD_MS     10u
#define MAX_HOLD_MS     10000u
#define MAX_FB          16

static volatile bool g_enabled = false;     /* Armed state, persisted across title launches */
static volatile bool g_menu = false;
static volatile bool g_capture = false;
static volatile bool g_isFortnite = false;
static volatile bool g_padHooksInstalled = false;
static volatile bool g_videoHooksInstalled = false;

#define FN_TITLE_ID "CUSA07669"
#define ARM_STATE_PATH "/data/GoldHEN/fn_menu_hider_state.bin"

static int      g_sel = 0;
static bool     g_unitMs = false;
static uint32_t g_intervalMs = 2000;
static uint32_t g_holdMs = 60;
static bool     g_holdMode = false; /* false = tap, true = hold */
static uint32_t g_target = BTN_CROSS;

static uint32_t g_prev = 0;
static uint64_t g_nextClick = 0;
static uint64_t g_pressUntil = 0;

/* Registered video framebuffer addresses. */
static void  *g_fbAddr[MAX_FB];
static size_t g_fbSize = 0;

typedef int (*scePadReadState_t)(int32_t, OrbisPadData *);
typedef int (*scePadRead_t)(int32_t, OrbisPadData *, int32_t);
typedef int32_t (*sceVideoOutRegisterBuffers_t)(
    int32_t, int32_t, void *const *, int32_t,
    const OrbisVideoOutBufferAttribute *);
typedef int32_t (*sceVideoOutSubmitFlip_t)(
    int32_t, int32_t, uint32_t, int64_t);

static Detour g_dReadState;
static Detour g_dRead;
static Detour g_dRegisterBuffers;
static Detour g_dSubmitFlip;

static scePadReadState_t orig_scePadReadState;
static scePadRead_t orig_scePadRead;
static sceVideoOutRegisterBuffers_t orig_sceVideoOutRegisterBuffers;
static sceVideoOutSubmitFlip_t orig_sceVideoOutSubmitFlip;

static bool is_fortnite_process(void) {
    OrbisAppInfo info;
    memset(&info, 0, sizeof(info));

    if (sceKernelGetAppInfo(getpid(), &info) != 0)
        return false;

    return strncmp(info.TitleId, FN_TITLE_ID, sizeof(info.TitleId)) == 0;
}

static void load_armed_state(void) {
    int fd = sceKernelOpen(ARM_STATE_PATH, 0, 0);
    uint8_t value = 0;

    if (fd >= 0) {
        if (sceKernelRead(fd, &value, sizeof(value)) == (ssize_t)sizeof(value))
            g_enabled = value != 0;
        sceKernelClose(fd);
    }
}

static void save_armed_state(void) {
    int fd = sceKernelOpen(ARM_STATE_PATH, 0x200 | 0x001, 0777);
    uint8_t value = g_enabled ? 1u : 0u;

    if (fd >= 0) {
        (void)sceKernelWrite(fd, &value, sizeof(value));
        sceKernelClose(fd);
    }
}

static void reset_click_timers(uint64_t now) {
    g_pressUntil = 0;
    g_nextClick = now + (uint64_t)g_intervalMs * 1000ull;
}

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
    char buf[384];
    char delay[32];
    char hold[32];

    if (g_capture) {
        snprintf(buf, sizeof(buf),
                 "== AUTO CLICKER ==\nPRESS ANY BUTTON TO BIND...");
        notify(buf);
        return;
    }

    if (g_unitMs) {
        snprintf(delay, sizeof(delay), "%u ms", (unsigned)g_intervalMs);
        snprintf(hold, sizeof(hold), "%u ms", (unsigned)g_holdMs);
    } else {
        snprintf(delay, sizeof(delay), "%u s",
                 (unsigned)(g_intervalMs / 1000u));
        snprintf(hold, sizeof(hold), "%u s",
                 (unsigned)((g_holdMs + 500u) / 1000u));
    }

    snprintf(buf, sizeof(buf),
             "== AUTO CLICKER MENU ==\n"
             "%s Target: Cross (X)\n"
             "%s Unit: %s\n"
             "%s Repeat delay: %s\n"
             "%s X action: %s\n"
             "%s X press time: %s\n"
             "(Up/Down select, Left/Right adjust, L1+R1 close)",
             g_sel == 0 ? ">" : " ",
             g_sel == 1 ? ">" : " ",
             g_unitMs ? "ms" : "s",
             g_sel == 2 ? ">" : " ",
             delay,
             g_sel == 3 ? ">" : " ",
             g_holdMode ? "Hold" : "Tap",
             g_sel == 4 ? ">" : " ",
             hold);
    notify(buf);
}

static void adjust_repeat_delay(int dir) {
    if (g_unitMs) {
        int64_t v = (int64_t)g_intervalMs + (int64_t)dir * 50;
        if (v < MIN_INTERVAL_MS) v = MIN_INTERVAL_MS;
        if (v > MAX_INTERVAL_MS) v = MAX_INTERVAL_MS;
        g_intervalMs = (uint32_t)v;
    } else {
        int64_t s = (int64_t)(g_intervalMs / 1000u) + dir;
        if (s < 1) s = 1;
        if (s > 600) s = 600;
        g_intervalMs = (uint32_t)s * 1000u;
    }
}

static void toggle_hold_mode(void) {
    g_holdMode = !g_holdMode;
}

static void adjust_hold_time(int dir) {
    if (g_unitMs) {
        int64_t v = (int64_t)g_holdMs + (int64_t)dir * 10;
        if (v < MIN_HOLD_MS) v = MIN_HOLD_MS;
        if (v > MAX_HOLD_MS) v = MAX_HOLD_MS;
        g_holdMs = (uint32_t)v;
    } else {
        int64_t s = (int64_t)((g_holdMs + 500u) / 1000u) + dir;
        if (s < 1) s = 1;
        if (s > 10) s = 10;
        g_holdMs = (uint32_t)s * 1000u;
    }
}

static void toggle_unit(void) {
    g_unitMs = !g_unitMs;

    if (!g_unitMs) {
        uint32_t s = (g_intervalMs + 500u) / 1000u;
        if (s < 1) s = 1;
        g_intervalMs = s * 1000u;
    }
}

/*
 * Input state is observed, not consumed, while blackout/autoclick is active.
 * The menu is the only state that intentionally suppresses game input.
 */
static void handle_input(uint32_t phys, uint64_t now) {
    uint32_t pressed = phys & ~g_prev;
    g_prev = phys;

    const bool l1 = (phys & BTN_L1) != 0;
    const bool r1 = (phys & BTN_R1) != 0;

    if (l1 && r1 && (pressed & (BTN_L1 | BTN_R1))) {
        if (g_capture) g_capture = false;
        g_menu = !g_menu;

        if (g_menu) {
            show_menu();
        } else {
            reset_click_timers(now);
            notify(g_enabled ? "Blackout + auto-click: ARMED"
                             : "Blackout + auto-click: DISARMED");
        }
        return;
    }

    if (g_menu && g_capture) {
        if (pressed & BTN_CROSS) {
            g_target = BTN_CROSS;
            g_capture = false;
            show_menu();
        }
        return;
    }

    if (g_menu) {
        bool changed = false;

        if (pressed & BTN_UP) {
            g_sel = (g_sel + 4) % 5;
            changed = true;
        }
        if (pressed & BTN_DOWN) {
            g_sel = (g_sel + 1) % 5;
            changed = true;
        }

        if (pressed & BTN_CROSS) {
            if (g_sel == 0) {
                g_capture = true;
                changed = true;
            } else if (g_sel == 1) {
                toggle_unit();
                changed = true;
            }
        }

        if (pressed & BTN_LEFT) {
            if (g_sel == 2) {
                adjust_repeat_delay(-1);
                changed = true;
            } else if (g_sel == 3) {
                toggle_hold_mode();
                changed = true;
            } else if (g_sel == 4) {
                adjust_hold_time(-1);
                changed = true;
            } else if (g_sel == 1) {
                toggle_unit();
                changed = true;
            }
        }

        if (pressed & BTN_RIGHT) {
            if (g_sel == 2) {
                adjust_repeat_delay(+1);
                changed = true;
            } else if (g_sel == 3) {
                toggle_hold_mode();
                changed = true;
            } else if (g_sel == 4) {
                adjust_hold_time(+1);
                changed = true;
            } else if (g_sel == 1) {
                toggle_unit();
                changed = true;
            }
        }

        if (changed) show_menu();
        return;
    }

    /* Global blackout + startup-click toggle: L1 + D-pad Down. */
    if (l1 && (pressed & BTN_DOWN)) {
        g_enabled = !g_enabled;
        reset_click_timers(now);
        save_armed_state();

        notify(g_enabled ? "Blackout + auto-click: ARMED"
                         : "Blackout + auto-click: DISARMED");
    }
}

static uint32_t filter_buttons(uint32_t phys, uint64_t now) {
    /*
     * Keep ordinary Fortnite input intact when the menu is closed.
     * The synthetic target button is only added for the timed startup click.
     */
    uint32_t out = phys;

    if (g_isFortnite && g_enabled && !g_menu && !g_capture) {
        if (now >= g_nextClick) {
            g_pressUntil = now + (uint64_t)(g_holdMode ? g_holdMs : 10u) * 1000ull;
            g_nextClick = now + (uint64_t)g_intervalMs * 1000ull;
        }

        if (now < g_pressUntil)
            out |= g_target;
    }

    return out;
}

static int hk_scePadReadState(int32_t handle, OrbisPadData *data) {
    if (!orig_scePadReadState)
        return -1;

    int ret = orig_scePadReadState(handle, data);

    if (ret == 0 && data && data->connected) {
        uint64_t now = sceKernelGetProcessTime();
        handle_input(data->buttons, now);
        data->buttons = filter_buttons(data->buttons, now);
    }

    return ret;
}

static int hk_scePadRead(int32_t handle, OrbisPadData *data, int32_t count) {
    if (!orig_scePadRead)
        return -1;

    int ret = orig_scePadRead(handle, data, count);

    if (ret > 0 && data) {
        uint64_t now = sceKernelGetProcessTime();

        for (int32_t i = 0; i < ret; ++i) {
            if (data[i].connected) {
                handle_input(data[i].buttons, now);
                data[i].buttons = filter_buttons(data[i].buttons, now);
            }
        }
    }

    return ret;
}

/*
 * Capture the addresses supplied by Fortnite's video-output registration.
 * We then clear the active framebuffer immediately before its flip.
 */
static int32_t hk_sceVideoOutRegisterBuffers(
    int32_t handle,
    int32_t startIndex,
    void *const *addresses,
    int32_t bufferNum,
    const OrbisVideoOutBufferAttribute *attr)
{
    if (!orig_sceVideoOutRegisterBuffers)
        return -1;

    int32_t ret = orig_sceVideoOutRegisterBuffers(
        handle, startIndex, addresses, bufferNum, attr);

    if (ret >= 0 && addresses && attr &&
        attr->width > 0 && attr->height > 0) {

        size_t w = ((size_t)attr->width + 31u) & ~(size_t)31u;
        size_t h = ((size_t)attr->height + 31u) & ~(size_t)31u;

        g_fbSize = w * h * 4u;

        for (int32_t i = 0; i < bufferNum; ++i) {
            int32_t idx = startIndex + i;

            if (idx >= 0 && idx < MAX_FB)
                g_fbAddr[idx] = addresses[i];
        }
    }

    return ret;
}

static int32_t hk_sceVideoOutSubmitFlip(
    int32_t handle,
    int32_t bufferIndex,
    uint32_t flipMode,
    int64_t flipArg)
{
    if (!orig_sceVideoOutSubmitFlip)
        return -1;

    /*
     * This is deliberately limited to the local display framebuffer.
     * No network/authentication/gameplay state is modified.
     */
    if (g_isFortnite &&
        g_enabled &&
        !g_menu &&
        !g_capture &&
        bufferIndex >= 0 &&
        bufferIndex < MAX_FB &&
        g_fbAddr[bufferIndex] &&
        g_fbSize) {

        memset(g_fbAddr[bufferIndex], 0, g_fbSize);
    }

    return orig_sceVideoOutSubmitFlip(
        handle, bufferIndex, flipMode, flipArg);
}

attr_public int module_start(size_t argc, const void *args) {
    (void)argc;
    (void)args;

    g_isFortnite = is_fortnite_process();
    load_armed_state();
    g_menu = false;
    g_capture = false;
    g_prev = 0;
    reset_click_timers(sceKernelGetProcessTime());

    /*
     * Detour_DetourFunction returns the original trampoline.
     * FunctionPtr MUST be uint64_t with this GoldHEN Detour API.
     */
    /*
     * The pad hooks are needed globally so the armed state can be changed
     * before launching Fortnite. Video hooks are title-gated because the
     * framebuffer ABI belongs to the target title.
     */
    Detour_Construct(&g_dReadState, DetourMode_x64);
    orig_scePadReadState =
        (scePadReadState_t)Detour_DetourFunction(
            &g_dReadState,
            (uint64_t)scePadReadState,
            (void *)hk_scePadReadState);

    Detour_Construct(&g_dRead, DetourMode_x64);
    orig_scePadRead =
        (scePadRead_t)Detour_DetourFunction(
            &g_dRead,
            (uint64_t)scePadRead,
            (void *)hk_scePadRead);
    g_padHooksInstalled = true;

    if (g_isFortnite) {
        Detour_Construct(&g_dRegisterBuffers, DetourMode_x64);
        orig_sceVideoOutRegisterBuffers =
            (sceVideoOutRegisterBuffers_t)Detour_DetourFunction(
                &g_dRegisterBuffers,
                (uint64_t)sceVideoOutRegisterBuffers,
                (void *)hk_sceVideoOutRegisterBuffers);

        Detour_Construct(&g_dSubmitFlip, DetourMode_x64);
        orig_sceVideoOutSubmitFlip =
            (sceVideoOutSubmitFlip_t)Detour_DetourFunction(
                &g_dSubmitFlip,
                (uint64_t)sceVideoOutSubmitFlip,
                (void *)hk_sceVideoOutSubmitFlip);
        g_videoHooksInstalled = true;
    }

    notify(g_isFortnite
        ? (g_enabled ? "FN Menu Hider: ARMED\nL1+Down toggle | L1+R1 menu"
                     : "FN Menu Hider: DISARMED\nL1+Down toggle | L1+R1 menu")
        : "FN Menu Hider: standby\nL1+Down arms/disarms");
    return 0;
}

attr_public int module_stop(size_t argc, const void *args) {
    (void)argc;
    (void)args;

    g_enabled = false;
    g_menu = false;
    g_capture = false;

    if (g_videoHooksInstalled) {
        Detour_Destroy(&g_dSubmitFlip);
        Detour_Destroy(&g_dRegisterBuffers);
        g_videoHooksInstalled = false;
    }

    if (g_padHooksInstalled) {
        Detour_Destroy(&g_dRead);
        Detour_Destroy(&g_dReadState);
        g_padHooksInstalled = false;
    }

    return 0;
}
