#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/VideoOut.h>
#include <Common.h>

attr_public const char *g_pluginName = "fn_hide";
attr_public const char *g_pluginDesc = "Fortnite Fullscreen Blackout + Menu Auto-Clicker";
attr_public const char *g_pluginAuth = "Custom";
attr_public uint32_t g_pluginVersion = 0x00000100;

// PS4 Pad Button Masks
#define BTN_L3       (1 << 1)
#define BTN_R3       (1 << 2)
#define BTN_OPTIONS  (1 << 3)
#define BTN_UP       (1 << 4)
#define BTN_RIGHT    (1 << 5)
#define BTN_DOWN     (1 << 6)
#define BTN_LEFT     (1 << 7)
#define BTN_L2       (1 << 8)
#define BTN_R2       (1 << 9)
#define BTN_L1       (1 << 10)
#define BTN_R1       (1 << 11)
#define BTN_TRIANGLE (1 << 12)
#define BTN_CIRCLE   (1 << 13)
#define BTN_CROSS    (1 << 14)
#define BTN_SQUARE   (1 << 15)

// Plugin State
static bool g_pluginActive = true;       // Starts ON at boot (blackout + auto-click)
static bool g_menuOpen = false;          // Menu Mode (L1 + R1)
static bool g_waitingForBind = false;    // True after pressing X on Target Button
static int g_menuIndex = 0;              // 0 = Target Button, 1 = Unit (s/ms), 2 = Delay

// Auto-Clicker Settings
static uint32_t g_targetButton = BTN_CROSS; // Default Cross (X)
static bool g_useMilliseconds = false;      // false = seconds, true = ms
static uint32_t g_delayValue = 2000;        // Default 2000 ms (2.0 s)

// VideoOut Buffer Tracking for Fullscreen Blackout
#define MAX_VIDEO_BUFFERS 16
static void *g_bufferAddrs[MAX_VIDEO_BUFFERS] = {0};
static size_t g_bufferSize = 1920 * 1080 * 4;

// Timing & Edge Detection
static uint64_t g_lastClickTimeUs = 0;
static uint64_t g_lastMenuMoveUs = 0;
static uint32_t g_prevButtons = 0;

HOOK_INIT(scePadReadState);
HOOK_INIT(scePadRead);
HOOK_INIT(sceVideoOutRegisterBuffers);
HOOK_INIT(sceVideoOutSubmitFlip);

static const char* get_button_name(uint32_t btn) {
    switch (btn) {
        case BTN_CROSS:    return "Cross (X)";
        case BTN_TRIANGLE: return "Triangle";
        case BTN_CIRCLE:   return "Circle";
        case BTN_SQUARE:   return "Square";
        case BTN_L1:       return "L1";
        case BTN_R1:       return "R1";
        case BTN_L2:       return "L2";
        case BTN_R2:       return "R2";
        case BTN_L3:       return "L3";
        case BTN_R3:       return "R3";
        case BTN_UP:       return "D-Pad Up";
        case BTN_DOWN:     return "D-Pad Down";
        case BTN_LEFT:     return "D-Pad Left";
        case BTN_RIGHT:    return "D-Pad Right";
        case BTN_OPTIONS:  return "Options";
        default:           return "Custom Button";
    }
}

static void show_osd_status(const char *msg) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.unk3 = 0;
    req.useIconImageUri = 0;
    req.targetId = -1;
    snprintf(req.message, sizeof(req.message), "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void show_menu_osd(void) {
    char buf[256];
    char delayStr[32];

    if (g_useMilliseconds) {
        snprintf(delayStr, sizeof(delayStr), "%u ms", g_delayValue);
    } else {
        snprintf(delayStr, sizeof(delayStr), "%.1f s", (float)g_delayValue / 1000.0f);
    }

    if (g_waitingForBind) {
        snprintf(buf, sizeof(buf), "[MENU] PRESS ANY BUTTON TO BIND...");
    } else {
        snprintf(buf, sizeof(buf),
            "[FN HIDE MENU]\n%s Button: %s\n%s Unit: %s\n%s Delay: %s",
            (g_menuIndex == 0) ? ">" : " ", get_button_name(g_targetButton),
            (g_menuIndex == 1) ? ">" : " ", g_useMilliseconds ? "ms" : "s",
            (g_menuIndex == 2) ? ">" : " ", delayStr);
    }
    show_osd_status(buf);
}

int32_t sceVideoOutRegisterBuffers_hook(int32_t handle, int32_t startIndex, void * const *addresses, int32_t bufferNum, const OrbisVideoOutBufferAttribute *attribute) {
    if (addresses && bufferNum > 0) {
        for (int i = 0; i < bufferNum && (startIndex + i) < MAX_VIDEO_BUFFERS; i++) {
            g_bufferAddrs[startIndex + i] = addresses[i];
        }
    }
    if (attribute && attribute->width > 0 && attribute->height > 0) {
        uint32_t pitch = (attribute->pitchInPixel > 0) ? attribute->pitchInPixel : attribute->width;
        g_bufferSize = (size_t)pitch * (size_t)attribute->height * 4;
    }
    return HOOK_CONTINUE(sceVideoOutRegisterBuffers, int32_t (*)(int32_t, int32_t, void * const *, int32_t, const OrbisVideoOutBufferAttribute *), handle, startIndex, addresses, bufferNum, attribute);
}

int32_t sceVideoOutSubmitFlip_hook(int32_t handle, int32_t bufferIndex, uint32_t flipMode, int64_t flipArg) {
    if (g_pluginActive && !g_menuOpen && bufferIndex >= 0 && bufferIndex < MAX_VIDEO_BUFFERS) {
        void *fb = g_bufferAddrs[bufferIndex];
        if (fb && g_bufferSize > 0) {
            memset(fb, 0x00, g_bufferSize);
        }
    }
    return HOOK_CONTINUE(sceVideoOutSubmitFlip, int32_t (*)(int32_t, int32_t, uint32_t, int64_t), handle, bufferIndex, flipMode, flipArg);
}

static void process_pad_data(OrbisPadData *data) {
    if (!data || !data->connected) return;

    uint32_t raw = data->buttons;
    uint32_t newlyPressed = raw & ~g_prevButtons;
    g_prevButtons = raw;

    uint64_t nowUs = sceKernelGetProcessTime();

    // 1. Toggle ON/OFF: L1 + D-Pad Down
    if ((raw & BTN_L1) && (newlyPressed & BTN_DOWN)) {
        g_pluginActive = !g_pluginActive;
        if (!g_pluginActive) {
            g_menuOpen = false;
            g_waitingForBind = false;
        }
        show_osd_status(g_pluginActive ? "FN Blackout + Auto-Click: ON" : "FN Blackout + Auto-Click: OFF");
        data->buttons &= ~(BTN_L1 | BTN_DOWN);
        return;
    }

    // 2. Toggle Menu Mode: L1 + R1
    if ((raw & BTN_L1) && (newlyPressed & BTN_R1)) {
        g_menuOpen = !g_menuOpen;
        g_waitingForBind = false;
        if (g_menuOpen) {
            show_menu_osd();
        } else {
            show_osd_status("Menu Closed");
        }
        data->buttons &= ~(BTN_L1 | BTN_R1);
        return;
    }

    // 3. Menu Mode Navigation & Interactive Button Capture
    if (g_menuOpen) {
        if (g_waitingForBind) {
            if (newlyPressed != 0) {
                g_targetButton = newlyPressed;
                g_waitingForBind = false;
                show_menu_osd();
            }
            data->buttons = 0;
            return;
        }

        if (newlyPressed & BTN_UP) {
            g_menuIndex = (g_menuIndex + 2) % 3;
            show_menu_osd();
        } else if (newlyPressed & BTN_DOWN) {
            g_menuIndex = (g_menuIndex + 1) % 3;
            show_menu_osd();
        }

        if (g_menuIndex == 0 && (newlyPressed & BTN_CROSS)) {
            g_waitingForBind = true;
            show_menu_osd();
        } else if (g_menuIndex == 1 && ((newlyPressed & BTN_CROSS) || (newlyPressed & BTN_LEFT) || (newlyPressed & BTN_RIGHT))) {
            g_useMilliseconds = !g_useMilliseconds;
            if (!g_useMilliseconds && g_delayValue < 500) {
                g_delayValue = 1000;
            }
            show_menu_osd();
        } else if (g_menuIndex == 2) {
            uint32_t step = g_useMilliseconds ? 50 : 500;
            if ((newlyPressed & BTN_RIGHT) || ((raw & BTN_RIGHT) && (nowUs - g_lastMenuMoveUs > 150000))) {
                g_delayValue += step;
                if (g_delayValue > 60000) g_delayValue = 60000;
                g_lastMenuMoveUs = nowUs;
                show_menu_osd();
            } else if ((newlyPressed & BTN_LEFT) || ((raw & BTN_LEFT) && (nowUs - g_lastMenuMoveUs > 150000))) {
                if (g_delayValue > step) {
                    g_delayValue -= step;
                } else {
                    g_delayValue = g_useMilliseconds ? 50 : 500;
                }
                g_lastMenuMoveUs = nowUs;
                show_menu_osd();
            }
        }

        data->buttons = 0;
        return;
    }

    // 4. Auto-Clicker Execution
    if (g_pluginActive) {
        uint64_t intervalUs = (uint64_t)g_delayValue * 1000ULL;
        uint64_t elapsedUs = nowUs - g_lastClickTimeUs;

        if (elapsedUs >= intervalUs) {
            data->buttons |= g_targetButton;
            if (elapsedUs >= intervalUs + 60000ULL) {
                g_lastClickTimeUs = nowUs;
            }
        }
    }
}

int scePadReadState_hook(int32_t handle, OrbisPadData *data) {
    int ret = HOOK_CONTINUE(scePadReadState, int (*)(int32_t, OrbisPadData *), handle, data);
    if (ret == 0 && data) {
        process_pad_data(data);
    }
    return ret;
}

int scePadRead_hook(int32_t handle, OrbisPadData *data, int32_t num) {
    int ret = HOOK_CONTINUE(scePadRead, int (*)(int32_t, OrbisPadData *, int32_t), handle, data, num);
    if (ret > 0 && data) {
        for (int i = 0; i < ret; i++) {
            process_pad_data(&data[i]);
        }
    }
    return ret;
}

attr_public int module_start(size_t argc, const void *args) {
    HOOK(scePadReadState);
    HOOK(scePadRead);
    HOOK(sceVideoOutRegisterBuffers);
    HOOK(sceVideoOutSubmitFlip);
    g_lastClickTimeUs = sceKernelGetProcessTime();
    show_osd_status("fn_hide loaded: ON (L1+Down: Toggle | L1+R1: Menu)");
    return 0;
}

attr_public int module_stop(size_t argc, const void *args) {
    UNHOOK(scePadReadState);
    UNHOOK(scePadRead);
    UNHOOK(sceVideoOutRegisterBuffers);
    UNHOOK(sceVideoOutSubmitFlip);
    return 0;
}
