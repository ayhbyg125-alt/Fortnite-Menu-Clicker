// Fortnite Menu Hider - diagnostic GoldHEN plugin
// Global-load test build: Fortnite framebuffer rectangle + diagnostic menu.
// This build intentionally does NOT synthesize controller input.

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
#include "plugin_common.h"

#define attr_public __attribute__((visibility("default")))

attr_public const char *g_pluginName = "button_swap";
attr_public const char *g_pluginDesc = "Fortnite framebuffer rectangle + diagnostic menu";
attr_public const char *g_pluginAuth = "Fortnite-Menu-Hider";
attr_public uint32_t g_pluginVersion = 0x00000400;

#define BTN_UP        0x00000010u
#define BTN_RIGHT     0x00000020u
#define BTN_DOWN      0x00000040u
#define BTN_LEFT      0x00000080u
#define BTN_L1        0x00000400u
#define BTN_R1        0x00000800u
#define BTN_CROSS     0x00004000u
#define BTN_CIRCLE    0x00002000u

#define FN_TITLE_ID "CUSA07669"
#define ARM_STATE_PATH "/data/GoldHEN/fn_menu_hider_state.bin"
#define MAX_FB 16

/* Conservative test rectangle. These are deliberately compile-time constants
 * until the actual framebuffer format/layout is confirmed on hardware. */
#define RECT_X       0u
#define RECT_Y       0u
#define RECT_WIDTH   320u
#define RECT_HEIGHT  120u

static volatile bool g_enabled = false;
static volatile bool g_menu = false;
static volatile bool g_isFortnite = false;
static volatile bool g_padHooksInstalled = false;
static volatile bool g_videoHooksInstalled = false;

static int g_sel = 0;
/* Runtime rectangle dimensions. Changes are kept in RAM for this plugin load. */
static uint32_t g_rectWidth = 320u;
static uint32_t g_rectHeight = 120u;
static uint32_t g_prev = 0;

static void *g_fbAddr[MAX_FB];
static size_t g_fbSize = 0;
static size_t g_fbWidth = 0;
static size_t g_fbHeight = 0;

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
    struct proc_info procInfo;
    memset(&procInfo, 0, sizeof(procInfo));

    if (sys_sdk_proc_info(&procInfo) != 0)
        return false;

    return strncmp(procInfo.titleid, FN_TITLE_ID, sizeof(procInfo.titleid)) == 0;
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

static void show_menu(void) {
    char buf[384];

    snprintf(buf, sizeof(buf),
        "== FN MENU HIDER ==\n"
        "%s Rectangle: %s\n"
        "%s Width: %u\n"
        "%s Height: %u\n"
        "%s Status: %s\n"
        "Up/Down select | Left/Right size | X toggle | O close\n"
        "L1+R1 open/close",
        g_sel == 0 ? ">" : " ",
        g_enabled ? "ON" : "OFF",
        g_sel == 1 ? ">" : " ",
        g_rectWidth,
        g_sel == 2 ? ">" : " ",
        g_rectHeight,
        g_sel == 3 ? ">" : " ",
        g_isFortnite ? "FORTNITE" : "STANDBY");

    notify(buf);
}

static void handle_input(uint32_t phys) {
    uint32_t pressed = phys & ~g_prev;
    g_prev = phys;

    bool l1 = (phys & BTN_L1) != 0;
    bool r1 = (phys & BTN_R1) != 0;

    if (l1 && r1 && (pressed & (BTN_L1 | BTN_R1))) {
        g_menu = !g_menu;

        if (g_menu) {
            show_menu();
        } else {
            notify(g_enabled
                ? "FN Menu Hider: RECTANGLE ON"
                : "FN Menu Hider: RECTANGLE OFF");
        }
        return;
    }

    if (!g_menu) {
        /* Observe Home/controller input only; never rewrite the pad data. */
        if (l1 && (pressed & BTN_DOWN)) {
            g_enabled = !g_enabled;
            save_armed_state();

            notify(g_enabled
                ? "FN Menu Hider: RECTANGLE ARMED"
                : "FN Menu Hider: RECTANGLE DISARMED");
        }
        return;
    }

    if (pressed & BTN_UP) {
        g_sel = (g_sel + 1) % 2;
        show_menu();
    }

    if (pressed & BTN_DOWN) {
        g_sel = (g_sel + 1) % 2;
        show_menu();
    }

    if (pressed & BTN_CIRCLE) {
        g_menu = false;
        notify("FN Menu Hider: MENU CLOSED");
        return;
    }

    if (pressed & BTN_CROSS) {
        if (g_sel == 0) {
            g_enabled = !g_enabled;
            save_armed_state();
        }

        show_menu();
    }
}

static int hk_scePadReadState(int32_t handle, OrbisPadData *data) {
    if (!orig_scePadReadState)
        return -1;

    int ret = orig_scePadReadState(handle, data);

    if (ret == 0 && data && data->connected)
        handle_input(data->buttons);

    return ret;
}

static int hk_scePadRead(int32_t handle, OrbisPadData *data, int32_t count) {
    if (!orig_scePadRead)
        return -1;

    int ret = orig_scePadRead(handle, data, count);

    if (ret > 0 && data) {
        for (int32_t i = 0; i < ret; ++i) {
            if (data[i].connected)
                handle_input(data[i].buttons);
        }
    }

    return ret;
}

static void draw_rectangle(uint8_t *fb) {
    if (!fb || !g_fbSize || !g_fbWidth || !g_fbHeight)
        return;

    size_t x0 = RECT_X;
    size_t y0 = RECT_Y;
    size_t x1 = x0 + RECT_WIDTH;
    size_t y1 = y0 + RECT_HEIGHT;

    if (x0 >= g_fbWidth || y0 >= g_fbHeight)
        return;

    if (x1 > g_fbWidth)
        x1 = g_fbWidth;
    if (y1 > g_fbHeight)
        y1 = g_fbHeight;

    /*
     * The existing blackout path already relies on a 4-byte linear
     * framebuffer. Preserve that known assumption for this first rectangle
     * test rather than introducing an unverified pixel-format conversion.
     */
    for (size_t y = y0; y < y1; ++y) {
        size_t row_offset = y * g_fbWidth * 4u;
        size_t rect_offset = row_offset + x0 * 4u;
        size_t rect_bytes = (x1 - x0) * 4u;

        if (rect_offset >= g_fbSize)
            break;

        if (rect_bytes > g_fbSize - rect_offset)
            rect_bytes = g_fbSize - rect_offset;

        memset(fb + rect_offset, 0, rect_bytes);
    }
}

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

        size_t w = (size_t)attr->width;
        size_t h = (size_t)attr->height;

        if (h > SIZE_MAX / 4u || w > SIZE_MAX / (h * 4u)) {
            g_fbSize = 0;
            g_fbWidth = 0;
            g_fbHeight = 0;
            memset(g_fbAddr, 0, sizeof(g_fbAddr));
            return ret;
        }

        g_fbWidth = w;
        g_fbHeight = h;
        g_fbSize = w * h * 4u;
        memset(g_fbAddr, 0, sizeof(g_fbAddr));

        int32_t count = bufferNum;
        if (count < 0) count = 0;
        if (count > MAX_FB) count = MAX_FB;

        for (int32_t i = 0; i < count; ++i) {
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

    if (g_isFortnite &&
        g_enabled &&
        !g_menu &&
        bufferIndex >= 0 &&
        bufferIndex < MAX_FB &&
        g_fbAddr[bufferIndex] &&
        g_fbSize) {

        draw_rectangle((uint8_t *)g_fbAddr[bufferIndex]);
    }

    return orig_sceVideoOutSubmitFlip(
        handle, bufferIndex, flipMode, flipArg);
}

static void install_hooks(void) {
    if (!g_padHooksInstalled) {
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
    }

    if (g_isFortnite && !g_videoHooksInstalled) {
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
}

static void uninstall_hooks(void) {
    if (g_videoHooksInstalled) {
        Detour_Destroy(&g_dSubmitFlip);
        Detour_Destroy(&g_dRegisterBuffers);
        g_videoHooksInstalled = false;
        orig_sceVideoOutSubmitFlip = NULL;
        orig_sceVideoOutRegisterBuffers = NULL;
    }

    if (g_padHooksInstalled) {
        Detour_Destroy(&g_dRead);
        Detour_Destroy(&g_dReadState);
        g_padHooksInstalled = false;
        orig_scePadRead = NULL;
        orig_scePadReadState = NULL;
    }
}

attr_public int plugin_load(int argc, char **argv) {
    (void)argc;
    (void)argv;

    g_isFortnite = is_fortnite_process();
    load_armed_state();
    g_menu = false;
    g_prev = 0;
    g_fbSize = 0;
    g_fbWidth = 0;
    g_fbHeight = 0;
    memset(g_fbAddr, 0, sizeof(g_fbAddr));

    install_hooks();

    notify(g_isFortnite
        ? "FN Menu Hider: LOADED\nL1+R1 = menu | L1+Down = rectangle"
        : "FN Menu Hider: STANDBY\nL1+Down = arm/disarm");

    return 0;
}

attr_public int plugin_unload(int argc, char **argv) {
    (void)argc;
    (void)argv;

    uninstall_hooks();
    g_menu = false;
    g_prev = 0;
    g_fbSize = 0;
    g_fbWidth = 0;
    g_fbHeight = 0;
    memset(g_fbAddr, 0, sizeof(g_fbAddr));

    return 0;
}

attr_module_hidden int module_start(size_t argc, const void *args) {
    (void)argc;
    (void)args;
    return 0;
}

attr_module_hidden int module_stop(size_t argc, const void *args) {
    (void)argc;
    (void)args;
    return 0;
}
