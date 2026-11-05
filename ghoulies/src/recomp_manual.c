/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;
/* Thread-local in the runtime, so it has to be declared that way here too. */
#ifdef _MSC_VER
extern __declspec(thread) uint32_t g_esp;
extern __declspec(thread) uint32_t g_eax;
extern __declspec(thread) uint32_t g_ecx;
#else
extern __thread uint32_t g_esp;
extern __thread uint32_t g_eax;
extern __thread uint32_t g_ecx;
#endif
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;

/* ── Grabbed by the Ghoulies: functions replaced by hand ──────────────────
 *
 * The recompiler skips generating any sub_XXXXXXXX defined here (the loop
 * script passes --exclude-manual), so these replace the game's own code for
 * every caller. Started from the generic parts of the Orta port; nothing in
 * here uses an Orta address.
 */
#define GMEM8(a)  (*(volatile uint8_t  *)(g_xbox_mem_offset + (uintptr_t)(uint32_t)(a)))
#define GMEM16(a) (*(volatile uint16_t *)(g_xbox_mem_offset + (uintptr_t)(uint32_t)(a)))
#define GMEM32(a) (*(volatile uint32_t *)(g_xbox_mem_offset + (uintptr_t)(uint32_t)(a)))

#ifdef _WIN32
__declspec(dllimport) void * __stdcall LoadLibraryA(const char *);
__declspec(dllimport) void * __stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) short  __stdcall GetAsyncKeyState(int);
__declspec(dllimport) void * __stdcall GetForegroundWindow(void);
__declspec(dllimport) unsigned long __stdcall GetWindowThreadProcessId(void *, unsigned long *);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
#pragma comment(lib, "user32.lib")

typedef struct {
    uint32_t packet;
    uint16_t buttons;
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
} WinPadState;
typedef unsigned long (__stdcall *WinXInputGetState)(unsigned long, WinPadState *);

static WinXInputGetState game_win_xinput(void)
{
    static int tried;
    static WinXInputGetState fn;
    if (!tried) {
        const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        int i;
        tried = 1;
        for (i = 0; i < 3 && !fn; i++) {
            void *m = LoadLibraryA(dlls[i]);
            if (m)
                fn = (WinXInputGetState)GetProcAddress(m, "XInputGetState");
        }
        fprintf(stderr, "[PAD] Windows XInput %s\n", fn ? "found" : "not found (keyboard only)");
    }
    return fn;
}

static int game_has_focus(void)
{
    unsigned long pid = 0;
    void *w = GetForegroundWindow();
    if (!w)
        return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
#endif

#include <stdlib.h>
#include <string.h>
#include "overlay.h"

/* ── Rumble ──────────────────────────────────────────────────────────────
 * XInputSetState's XINPUT_FEEDBACK is a 66-byte header (dwStatus, hEvent,
 * 58 reserved bytes) and then XINPUT_RUMBLE: left (heavy) and right (light)
 * motor speeds, 0..65535 -- the same pair Windows takes. The game's last
 * request is kept and sent on, scaled by settings.ini Rumble /
 * RumbleStrength (or the menu), and held at zero while the window is in
 * the background or the menu is open. */
volatile float g_rumble_scale = -1.0f;      /* -1: from settings.ini */
static uint16_t s_rum_want[2];              /* what the game asked for */
static int      s_rum_sent[2] = { -1, -1 }; /* what the pad was last told */

#ifdef _WIN32
typedef struct { uint16_t l, r; } WinVib;
typedef unsigned long (__stdcall *WinXInputSetState)(unsigned long, WinVib *);

static WinXInputSetState game_win_xinput_set(void)
{
    static int tried;
    static WinXInputSetState fn;
    if (!tried) {
        const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        int i;
        tried = 1;
        for (i = 0; i < 3 && !fn; i++) {
            void *m = LoadLibraryA(dlls[i]);
            if (m)
                fn = (WinXInputSetState)GetProcAddress(m, "XInputSetState");
        }
    }
    return fn;
}
#endif

static float game_rumble_k(void)
{
    static float k = -1.0f;
    if (g_rumble_scale >= 0.0f)
        return g_rumble_scale;
    if (k < 0.0f) {
        const char *on = getenv("RECOMP_RUMBLE"), *st = getenv("RECOMP_RUMBLE_STRENGTH");
        int s = st ? atoi(st) : 100;
        if (s < 0) s = 0;
        if (s > 100) s = 100;
        k = (on && *on == '0') ? 0.0f : (float)s / 100.0f;
    }
    return k;
}

static void game_rumble_send(int l, int r)
{
#ifdef _WIN32
    WinXInputSetState xs = game_win_xinput_set();
    if (xs && (l != s_rum_sent[0] || r != s_rum_sent[1])) {
        WinVib v;
        v.l = (uint16_t)l;
        v.r = (uint16_t)r;
        xs(0, &v);
        s_rum_sent[0] = l;
        s_rum_sent[1] = r;
    }
#else
    (void)l; (void)r;
#endif
}

static void game_rumble_exit(void)
{
    s_rum_sent[0] = s_rum_sent[1] = -1;
    game_rumble_send(0, 0);
}

/* Bring the pad in line with the game's request; every game frame. */
static void game_rumble_update(void)
{
    static int hooked;
    float k = game_rumble_k();
    int quiet = ovl_is_open();
#ifdef _WIN32
    quiet = quiet || !game_has_focus();
#endif
    if (!hooked) {
        hooked = 1;
        atexit(game_rumble_exit);
    }
    if (quiet || k <= 0.0f)
        game_rumble_send(0, 0);
    else
        game_rumble_send((int)(s_rum_want[0] * k), (int)(s_rum_want[1] * k));
}

/* For the menu: stop now (and resend on the next game frame), or a test pulse. */
void game_rumble_stop_now(void)
{
    s_rum_sent[0] = s_rum_sent[1] = -1;
    game_rumble_send(0, 0);
}

void game_rumble_pulse(float strength)
{
    int v = (int)(65535.0f * (strength < 0 ? 0 : strength > 1 ? 1 : strength));
    s_rum_sent[0] = s_rum_sent[1] = -1;
    game_rumble_send(v, v);
}

/* Fill an Xbox XINPUT_GAMEPAD (18 bytes) at guest address `g` for game port
 * `port` (0-3): Windows pad `port`, plus the keyboard on port 0. */
static void game_read_pad(uint32_t g, int port)
{
    uint16_t buttons = 0;
    uint8_t an[8] = {0};
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    int i;
#ifdef _WIN32
    WinXInputGetState xi = game_win_xinput();
    WinPadState ws;
    if (xi && xi((unsigned long)port, &ws) == 0) {
        buttons = ws.buttons & 0x00FF;          /* d-pad, start, back, sticks */
        an[0] = (ws.buttons & 0x1000) ? 0xFF : 0; /* A */
        an[1] = (ws.buttons & 0x2000) ? 0xFF : 0; /* B */
        an[2] = (ws.buttons & 0x4000) ? 0xFF : 0; /* X */
        an[3] = (ws.buttons & 0x8000) ? 0xFF : 0; /* Y */
        an[4] = (ws.buttons & 0x0200) ? 0xFF : 0; /* Black = RB */
        an[5] = (ws.buttons & 0x0100) ? 0xFF : 0; /* White = LB */
        an[6] = ws.lt;
        an[7] = ws.rt;
        lx = ws.lx; ly = ws.ly; rx = ws.rx; ry = ws.ry;
    }
    if (port == 0 && game_has_focus()) {
        #define KEY(vk) (GetAsyncKeyState(vk) & 0x8000)
        if (KEY(0x26)) buttons |= 0x01;             /* Up    */
        if (KEY(0x28)) buttons |= 0x02;             /* Down  */
        if (KEY(0x25)) buttons |= 0x04;             /* Left  */
        if (KEY(0x27)) buttons |= 0x08;             /* Right */
        if (KEY(0x0D)) buttons |= 0x10;             /* Enter = Start */
        if (KEY(0x1B) || KEY(0x08)) buttons |= 0x20;/* Esc/Backspace = Back */
        if (KEY('Z')) an[0] = 0xFF;
        if (KEY('X')) an[1] = 0xFF;
        if (KEY('A')) an[2] = 0xFF;
        if (KEY('S')) an[3] = 0xFF;
        if (KEY('V')) an[4] = 0xFF;
        if (KEY('C')) an[5] = 0xFF;
        if (KEY('Q')) an[6] = 0xFF;
        if (KEY('W')) an[7] = 0xFF;
        #undef KEY
    }
#endif
    {   /* The settings menu takes the controls while it is up, and until
         * everything is let go after it closes. */
        int held = buttons != 0 || lx > 8000 || lx < -8000 || ly > 8000 || ly < -8000
                || rx > 8000 || rx < -8000 || ry > 8000 || ry < -8000;
        for (i = 0; i < 8; i++)
            held |= an[i] > 0x40;
        if (ovl_block_input(held)) {
            buttons = 0;
            memset(an, 0, sizeof an);
            lx = ly = rx = ry = 0;
        }
    }
    if (port == 0)
        game_rumble_update();
    GMEM16(g) = buttons;
    for (i = 0; i < 8; i++)
        GMEM8(g + 2 + i) = an[i];
    GMEM16(g + 10) = (uint16_t)lx;
    GMEM16(g + 12) = (uint16_t)ly;
    GMEM16(g + 14) = (uint16_t)rx;
    GMEM16(g + 16) = (uint16_t)ry;
}


/* ── Controller: Xbox XInput answered from the PC ──────────────────────────
 *
 * The USB stack is skipped, so the game's controller calls (statically
 * linked XAPI 5659, XPP section) are replaced here. Ports 1-4 are Windows
 * XInput pads 1-4; the keyboard also drives port 1 when the window has focus:
 *
 *   Arrows = D-pad   Enter = Start   Esc/Backspace = Back
 *   Z = A   X = B   A = X   S = Y   Q = L trigger   W = R trigger
 *   C = White   V = Black
 *
 * Found in the Ghoulies XBE (all stdcall). The game's only caller is its
 * input set-up at 0x155540 / 0x1554D0 / 0x1555B0:
 *   0x1FDD30 XInitDevices(count, types)               ret 8 (jmp 0x1FD0E3)
 *   0x1FDD35 XGetDevices(type)                        ret 4
 *   0x1FDD57 XGetDeviceChanges(type, &ins, &rem)      ret 0xC
 *   0x1FDC06 XInputOpen(type, port, slot, params)     ret 0x10
 *   0x1FDC5C XInputClose(h)                           ret 4
 *   0x1FDC68 XInputGetState(h, state)                 ret 8
 *   0x1FDCDB XInputSetState(h, feedback)              ret 8
 * XInputGetCapabilities and XInputPoll are not linked into this game.
 * 0x1FCDC8 is XDEVICE_TYPE_GAMEPAD (the game keeps it at 0x32E3B0). */
#define GH_GAMEPAD_TYPE  0x001FCDC8u
#define GH_PAD_HANDLE    0x0C0FFEE0u     /* + port */

__declspec(dllimport) unsigned long __stdcall GetTickCount(void);

/* Ports with a pad: port 1 always (keyboard), 2-4 when a Windows pad is
 * there. Re-checked once a second: asking Windows about an empty slot is slow. */
static uint32_t gh_pad_mask(void)
{
    static uint32_t mask = 1, last;
    static int first = 1;
    uint32_t now = GetTickCount();
    if (first || now - last > 1000) {
        WinXInputGetState xi = game_win_xinput();
        WinPadState ws;
        int p;
        mask = 1;
        for (p = 1; p < 4 && xi; p++)
            if (xi((unsigned long)p, &ws) == 0)
                mask |= 1u << p;
        last = now;
        first = 0;
    }
    return mask;
}

static uint32_t s_pad_reported;   /* the mask the game was last told about */

/* XInitDevices(count, types): no USB host to bring up. */
static void gh_monitor_start(void);

void sub_001FDD30(void)
{
    gh_monitor_start();
    g_esp += 4 + 8;
}

/* XGetDevices(type): bitmask of ports with that device. */
void sub_001FDD35(void)
{
    uint32_t type = GMEM32(g_esp + 4);
    g_eax = 0;
    if (type == GH_GAMEPAD_TYPE) {
        g_eax = s_pad_reported = gh_pad_mask();
        fprintf(stderr, "[PAD] XGetDevices -> %X\n", g_eax);
    }
    g_esp += 4 + 4;
}

/* XGetDeviceChanges(type, &inserted, &removed) -> TRUE if anything changed. */
void sub_001FDD57(void)
{
    uint32_t type = GMEM32(g_esp + 4), pins = GMEM32(g_esp + 8), prem = GMEM32(g_esp + 12);
    uint32_t ins = 0, rem = 0;
    if (type == GH_GAMEPAD_TYPE) {
        uint32_t now = gh_pad_mask();
        ins = now & ~s_pad_reported;
        rem = s_pad_reported & ~now;
        s_pad_reported = now;
        if (ins | rem)
            fprintf(stderr, "[PAD] device changes: inserted %X removed %X\n", ins, rem);
    }
    GMEM32(pins) = ins;
    GMEM32(prem) = rem;
    g_eax = (ins | rem) ? 1u : 0u;
    g_esp += 4 + 12;
}

/* XInputOpen(type, port, slot, params) -> handle */
void sub_001FDC06(void)
{
    uint32_t type = GMEM32(g_esp + 4), port = GMEM32(g_esp + 8);
    static int logged;
    g_eax = (type == GH_GAMEPAD_TYPE && port < 4) ? GH_PAD_HANDLE + port : 0u;
    if (logged++ < 8)
        fprintf(stderr, "[PAD] XInputOpen type %08X port %u -> %08X\n", type, port, g_eax);
    g_esp += 4 + 16;
}

/* XInputClose(h) */
void sub_001FDC5C(void)
{
    g_esp += 4 + 4;
}

/* XInputGetState(h, state): dwPacketNumber, then XINPUT_GAMEPAD. */
/* ── Input recording and replay (port 1) ──────────────────────────────────
 *
 * RECOMP_INPUT_RECORD=<file>: every change of port 1's state is appended as
 *   "<swap> <18 bytes hex>", <swap> being D3D's count of completed frames.
 * RECOMP_INPUT_REPLAY=<file>: plays such a file back on port 1, so a test run
 *   can get from boot to the same place every time without anyone at the pad.
 *   Timing is kept in game frames, gap by gap; gaps longer than a second are
 *   stretched (RECOMP_INPUT_REPLAY_STRETCH, percent, default 150) because
 *   loading screens do not take the same number of frames every run, and a
 *   press that lands during a load is lost. Touching the pad or keyboard
 *   ends the replay and hands port 1 back.
 * Asked for by the user, for repeatable runs into the first level. */
#define GH_MAX_EV 4096
static uint32_t gh_ev_swap[GH_MAX_EV];
static uint8_t  gh_ev_state[GH_MAX_EV][18];
static int gh_ev_n = -1, gh_ev_i;
static uint32_t gh_ev_last_fire;
static uint8_t  gh_replay_state[18];
static int gh_replay_on;

static uint32_t gh_swaps(void)
{
    uint32_t dev = GMEM32(0x00181E08u);
    if (!dev || dev >= 0x04000000u)
        return 0;
    return GMEM32(dev + 0x1C18u + 0x1BCu);
}

static void gh_replay_load(void)
{
    const char *path = getenv("RECOMP_INPUT_REPLAY");
    FILE *f;
    char line[256];
    gh_ev_n = 0;
    if (!path || !*path || !(f = fopen(path, "r")))
        return;
    while (gh_ev_n < GH_MAX_EV && fgets(line, sizeof line, f)) {
        unsigned sw, b[18];
        int k;
        if (sscanf(line, "%u %2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x%2x", &sw,
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7], &b[8], &b[9],
                   &b[10], &b[11], &b[12], &b[13], &b[14], &b[15], &b[16], &b[17]) != 19)
            continue;
        gh_ev_swap[gh_ev_n] = sw;
        for (k = 0; k < 18; k++)
            gh_ev_state[gh_ev_n][k] = (uint8_t)b[k];
        gh_ev_n++;
    }
    fclose(f);
    gh_replay_on = gh_ev_n > 0;
    gh_ev_last_fire = 0;
    fprintf(stderr, "[INPUT] replaying %d input changes from %s\n", gh_ev_n, path);
}

/* Port 1's state for this poll: live, or the replay while it runs. */
static void gh_input_port1(uint8_t now[18])
{
    static FILE *rec;
    static int rec_tried;
    static uint8_t rec_last[18];
    static uint32_t stretch = 0;
    uint32_t sw = gh_swaps();
    int k, live = 0;

    /* sticks rest near zero: only a clear deflection counts as touching */
    if (now[0] || now[1]) live = 1;
    for (k = 2; k < 10; k++) if (now[k] > 0x40) live = 1;
    for (k = 10; k < 18; k += 2) {
        int16_t v = (int16_t)(now[k] | (now[k + 1] << 8));
        if (v > 12000 || v < -12000) live = 1;
    }

    if (!rec_tried) {
        const char *p = getenv("RECOMP_INPUT_RECORD");
        rec_tried = 1;
        if (p && *p && (rec = fopen(p, "w")) != NULL)
            fprintf(stderr, "[INPUT] recording port 1 to %s\n", p);
    }
    if (gh_ev_n < 0)
        gh_replay_load();
    if (!stretch) {
        const char *e = getenv("RECOMP_INPUT_REPLAY_STRETCH");
        stretch = e ? (uint32_t)atoi(e) : 150u;
        if (stretch < 100u) stretch = 100u;
    }

    if (gh_replay_on) {
        if (live) {
            gh_replay_on = 0;
            fprintf(stderr, "[INPUT] live input at frame %u: replay stopped after %d of %d"
                            " changes\n", sw, gh_ev_i, gh_ev_n);
        } else {
            while (gh_ev_i < gh_ev_n) {
                uint32_t gap = gh_ev_swap[gh_ev_i]
                             - (gh_ev_i ? gh_ev_swap[gh_ev_i - 1] : 0u);
                if (gap > 30u)
                    gap = gap * stretch / 100u;
                if (sw - gh_ev_last_fire < gap && gh_ev_i)
                    break;
                if (!gh_ev_i && sw < gap)
                    break;
                memcpy(gh_replay_state, gh_ev_state[gh_ev_i], 18);
                gh_ev_last_fire = sw;
                gh_ev_i++;
                {
                    static int said;
                    if (said++ < 200)
                        fprintf(stderr, "[INPUT] frame %u: replay %d/%d buttons %02X%02X"
                                        " A%02X B%02X X%02X Y%02X\n", sw, gh_ev_i, gh_ev_n,
                                gh_replay_state[1], gh_replay_state[0], gh_replay_state[2],
                                gh_replay_state[3], gh_replay_state[4], gh_replay_state[5]);
                }
            }
            if (gh_ev_i >= gh_ev_n) {
                int idle = 1;
                for (k = 0; k < 18; k++) idle &= gh_replay_state[k] == 0;
                if (idle || sw - gh_ev_last_fire > 60u) {
                    gh_replay_on = 0;
                    memset(gh_replay_state, 0, 18);
                    fprintf(stderr, "[INPUT] replay finished at frame %u -- port 1 is"
                                    " live again\n", sw);
                }
            }
            memcpy(now, gh_replay_state, 18);
        }
    }

    if (rec && memcmp(now, rec_last, 18)) {
        fprintf(rec, "%u", sw);
        for (k = 0; k < 18; k++)
            fprintf(rec, "%s%02X", k ? "" : " ", now[k]);
        fprintf(rec, "\n");
        fflush(rec);
        memcpy(rec_last, now, 18);
    }
}

void sub_001FDC68(void)
{
    uint32_t h = GMEM32(g_esp + 4), state = GMEM32(g_esp + 8);
    uint32_t port = h - GH_PAD_HANDLE;
    static uint32_t packet[4];
    static uint8_t last[4][18];
    static uint32_t calls;
    uint8_t now[18];
    int i;
    if (port >= 4) {
        g_eax = 0x48Fu;                       /* ERROR_DEVICE_NOT_CONNECTED */
        g_esp += 4 + 8;
        return;
    }
    game_read_pad(state + 4, (int)port);
    for (i = 0; i < 18; i++)
        now[i] = GMEM8(state + 4 + i);
    if (port == 0) {
        gh_input_port1(now);
        for (i = 0; i < 18; i++)
            GMEM8(state + 4 + i) = now[i];
    }
    if (memcmp(now, last[port], 18)) {
        static int shown;
        packet[port]++;
        if (shown++ < 40)
            fprintf(stderr, "[PAD] port %u input: buttons %02X%02X A%02X B%02X X%02X Y%02X\n",
                    port + 1, now[1], now[0], now[2], now[3], now[4], now[5]);
        memcpy(last[port], now, 18);
    }
    if (++calls == 1 || calls == 100 || calls == 1000)
        fprintf(stderr, "[PAD] XInputGetState called %u times\n", calls);
    GMEM32(state) = packet[port];
    g_eax = 0;
    g_esp += 4 + 8;
}

/* XInputSetState(h, feedback): rumble (port 1) to the PC pad; report done.
 * XINPUT_FEEDBACK: 66-byte header (dwStatus, hEvent, reserved), then the
 * left / right motor speeds. */
void sub_001FDCDB(void)
{
    uint32_t h = GMEM32(g_esp + 4), fb = GMEM32(g_esp + 8);
    static unsigned logged;
    if (h == GH_PAD_HANDLE) {
        s_rum_want[0] = GMEM16(fb + 66);
        s_rum_want[1] = GMEM16(fb + 68);
        if (logged < 12 && (s_rum_want[0] || s_rum_want[1] || logged < 2)) {
            logged++;
            fprintf(stderr, "[RUMBLE] left %u right %u (event %08X)\n",
                    s_rum_want[0], s_rum_want[1], GMEM32(fb + 4));
        }
        game_rumble_update();
    }
    GMEM32(fb) = 0;                           /* header.dwStatus = success */
    g_eax = 0;
    g_esp += 4 + 8;
}

/* ── DVD security check (XAPI start-up, 0x150E37) ─────────────────────────
 *
 * cdecl, no arguments, returns NTSTATUS. Opens \Device\CdRom0, sends the
 * drive's authentication MODE SENSE, then -- because this XDK (5659) build's
 * certificate asks for it -- reads the disc's security sector (READ DVD
 * STRUCTURE 0xAD, 0x664 bytes) and validates it (0x150D80). There is no
 * disc here, so the validation failed five times and the caller (0x151068)
 * sent the title back to the dashboard (HalReturnToFirmware) a second after
 * start. Orta's XDK (4928) never asked for this check. Report success. */
void sub_00150E37(void)
{
    static int logged;
    if (!logged++)
        fprintf(stderr, "[BOOT] DVD security check skipped (reported OK)\n");
    g_eax = 0;
    g_esp += 4;
}

/* ── XMountUtilityDrive(BOOL fFormatClusters) (0x14BFCF) ──────────────────
 *
 * stdcall, 1 argument, returns BOOL. Ghoulies' XBE asks for the cache
 * partition to be formatted on every boot (init flags 1|2), so this tried to
 * dismount and reformat Harddisk0\PartitionN (NtFsControlFile 0x90020, not
 * supported here), then checked for 16 KB clusters on the partition image,
 * failed both, and the title went back to the dashboard. The runtime already
 * maps Z: to its cache folder, so report the drive mounted. */
void sub_0014BFCF(void)
{
    fprintf(stderr, "[BOOT] XMountUtilityDrive(format=%u) skipped (Z: = runtime cache)\n",
            GMEM32(g_esp + 4));
    g_eax = 1;
    g_esp += 4 + 4;
}

/* ── Call a translated guest function from hand-written code ──────────────
 * Pushes `ret_va` as the return address the callee will pop. */
extern recomp_func_t recomp_lookup(uint32_t xbox_va);

static void game_call_guest(uint32_t va, uint32_t ret_va)
{
    recomp_func_t fn = recomp_lookup(va);
    g_esp -= 4; GMEM32(g_esp) = ret_va;
    if (fn)
        fn();
    else {
        fprintf(stderr, "[CALL] %08X not translated\n", va);
        g_esp += 4;
    }
}

/* ── DirectSound effects-image download wrapper (0x1C5A33) ───────────────
 *
 *   thiscall(obj, a1, a2, a3), ret 0xC:
 *     hr = DSP_MANAGER_0x1C98E6(obj->mgr (at +0x14), a1, a2, a3);
 *     obj->flags_0x80 = (hr >= 0) ? flags | 1 : flags & ~1;
 *
 * 0x1C98E6 ends in 0x1C9679, which posts command 3 to the GP DSP at
 * scratch+0x810 and spins until the DSP clears it. The DSP here is a stub,
 * so the runtime's DSP-ack clears it -- for addresses it has been told
 * about. The scratch block moves whenever DirectSound re-initialises, so
 * register the live address before every download. Same fix as Orta's
 * 0x2C1221 (where the flags were at +0x320). */
extern void xbox_DspAckAdd(uint32_t va);

void sub_001C5A33(void)
{
    uint32_t obj = g_ecx;
    uint32_t a1 = GMEM32(g_esp + 4), a2 = GMEM32(g_esp + 8), a3 = GMEM32(g_esp + 12);
    uint32_t mgr = GMEM32(obj + 0x14);
    uint32_t hr;
    static int logged;

    if (mgr) {
        uint32_t p = GMEM32(mgr + 8);
        if (p) p = GMEM32(p + 0x10);
        if (p) p = GMEM32(p);
        if (p) {
            xbox_DspAckAdd(p + 0x810);
            if (logged++ < 4)
                fprintf(stderr, "[DSOUND] effects download: DSP mailbox %08X registered\n", p + 0x810);
        }
    }
    g_esp -= 4; GMEM32(g_esp) = a3;
    g_esp -= 4; GMEM32(g_esp) = a2;
    g_esp -= 4; GMEM32(g_esp) = a1;
    g_ecx = mgr;
    game_call_guest(0x001C98E6u, 0x001C5A4Au);   /* callee pops ret + 12 */
    hr = g_eax;
    if ((int32_t)hr >= 0)
        GMEM32(obj + 0x80) |= 1u;
    else
        GMEM32(obj + 0x80) &= ~1u;
    g_eax = hr;
    g_esp += 4 + 12;
}

/* ── MMX check (0x162840) ─────────────────────────────────────────────────
 *
 * cdecl(flags), no return value. Bink calls it before every copy-out of a
 * video frame: it tests for CPUID by toggling EFLAGS.ID with pushfd/popfd,
 * asks CPUID for the MMX bit, caches that at 0x336894, then sets 0x54D9E0 to
 * "use the MMX copy routines" unless flags has bit 0x40000. The translated
 * pushfd/popfd do not model the ID bit, so it decided "no CPUID, no MMX",
 * Bink skipped choosing its copy routines, and the intro movie called a null
 * function pointer (0x1AF015). Every Xbox has MMX, so answer that. */
void sub_00162840(void)
{
    uint32_t flags = GMEM32(g_esp + 4);
    GMEM32(0x00336894u) = 1;
    GMEM32(0x0054D9E0u) = (flags & 0x40000u) ? 0u : 1u;
    g_esp += 4;
}

/* ── D3D "is the GPU busy?" (0x174B90) ────────────────────────────────────
 *
 * cdecl, no arguments, returns BOOL. The original reads PFIFO CACHE1
 * DMA_PUT/GET (+0x3240/+0x3244) and PGRAPH_STATUS (+0x400700) through the
 * register base the device keeps at +0x930. KickOff (0x177280) spins on it
 * after publishing PUT, and run 5 sat in that spin for the whole 600 s with
 * the GPU idle (USER PUT == GET). Answer from the state the runtime keeps up
 * to date instead: busy while the command reader (GET) is behind PUT. */
#define GH_NV2A_USER_PUT 0xFD800040u
#define GH_NV2A_USER_GET 0xFD800044u

void sub_00174B90(void)
{
    uint32_t put = GMEM32(GH_NV2A_USER_PUT), get = GMEM32(GH_NV2A_USER_GET);
    static unsigned calls, busy;
    g_eax = (put != get) ? 1u : 0u;
    calls++;
    if (g_eax) busy++;
    if (calls == 1 || calls == 1000 || calls == 100000)
        fprintf(stderr, "[GPU] busy check #%u: PUT %08X GET %08X (%u busy so far)\n",
                calls, put, get, busy);
    g_esp += 4;
}

/* ── Bring-up monitor ─────────────────────────────────────────────────────
 *
 * Every 10 s: the D3D device (global 0x181E08) and the fields the KickOff
 * path uses, and the GPU registers behind each base it might use. The
 * runtime's own peek tools stop at 0x04000000, and Ghoulies keeps these in
 * contiguous memory and the register aperture. Started from XInitDevices. */
__declspec(dllimport) void * __stdcall CreateThread(void *, size_t,
        unsigned long (__stdcall *)(void *), void *, unsigned long, unsigned long *);
__declspec(dllimport) void __stdcall Sleep(unsigned long);

static int gh_guest_ok(uint32_t va)
{
    return (va >= 0x1000u && va < 0x04000000u)
        || (va >= 0x80000000u && va < 0x84000000u)
        || (va >= 0xFD000000u && va < 0xFE000000u);
}

static void gh_dump_regs(const char *what, uint32_t base)
{
    if (!gh_guest_ok(base) || !gh_guest_ok(base + 0x400700u)) {
        fprintf(stderr, "[MON]   %s base %08X: not mapped\n", what, base);
        return;
    }
    fprintf(stderr, "[MON]   %s base %08X: 3240=%08X 3244=%08X 400700=%08X 100410=%08X 800040=%08X 800044=%08X\n",
            what, base, GMEM32(base + 0x3240u), GMEM32(base + 0x3244u),
            GMEM32(base + 0x400700u), GMEM32(base + 0x100410u),
            gh_guest_ok(base + 0x800044u) ? GMEM32(base + 0x800040u) : 0,
            gh_guest_ok(base + 0x800044u) ? GMEM32(base + 0x800044u) : 0);
}

static unsigned long __stdcall gh_monitor(void *arg)
{
    unsigned n = 0;
    (void)arg;
    for (;;) {
        uint32_t dev;
        Sleep(10000);
        if (++n > 120)
            return 0;
        dev = GMEM32(0x00181E08u);
        fprintf(stderr, "[MON] t=%us dev=%08X flag181960=%08X USER PUT=%08X GET=%08X\n",
                n * 10, dev, GMEM32(0x00181960u),
                GMEM32(GH_NV2A_USER_PUT), GMEM32(GH_NV2A_USER_GET));
        if (!gh_guest_ok(dev) || !gh_guest_ok(dev + 0x2470u))
            continue;
        {
            uint32_t sp = GMEM32(dev + 0x30u);
            fprintf(stderr, "[MON]   dev+0=%08X +4=%08X +8=%08X +24=%08X +28=%08X +2C=%08X +30=%08X(->%08X)"
                            " +930=%08X +1C10=%08X +1C18=%08X +1DBC=%08X +1DC0=%08X\n",
                    GMEM32(dev), GMEM32(dev + 4u), GMEM32(dev + 8u),
                    GMEM32(dev + 0x24u), GMEM32(dev + 0x28u), GMEM32(dev + 0x2Cu),
                    sp, gh_guest_ok(sp) ? GMEM32(sp) : 0,
                    GMEM32(dev + 0x930u), GMEM32(dev + 0x1C10u), GMEM32(dev + 0x1C18u),
                    GMEM32(dev + 0x1DBCu), GMEM32(dev + 0x1DC0u));
        }
        gh_dump_regs("+930", GMEM32(dev + 0x930u));
        gh_dump_regs("+1C18", GMEM32(dev + 0x1C18u));
        /* The loading-screen icon textures (32x32 DXT3, 1 KB each) sample as
         * alpha 0 everywhere: are they empty, or full of something? The same
         * page is also read through the tiled alias (0xF0000000) in case the
         * game wrote them that way. */
        /* One-off memory dump at 20 s for offline analysis (fonts, glyph
         * textures, unpacked bundles): the contiguous window and the low
         * 64 MB, written next to the game as memdump_*.bin. */
        if (n == 2 && getenv("RECOMP_MEMDUMP") && *getenv("RECOMP_MEMDUMP") == '1') {
            static const struct { uint32_t va, size; const char *name; } dumps[] = {
                { 0x80000000u, 0x04000000u, "memdump_contig.bin" },
                { 0x00000000u, 0x04000000u, "memdump_low.bin" },
            };
            int dd;
            for (dd = 0; dd < 2; dd++) {
                FILE *df = fopen(dumps[dd].name, "wb");
                uint32_t off;
                if (!df) continue;
                for (off = 0; off < dumps[dd].size; off += 0x10000u) {
                    uint32_t va = dumps[dd].va + off;
                    if (va < 0x10000u) {            /* unmapped page zero */
                        static uint8_t zeros[0x10000];
                        fwrite(zeros, 1, sizeof zeros, df);
                    } else
                        fwrite((const void *)(g_xbox_mem_offset + (uintptr_t)va), 1, 0x10000u, df);
                }
                fclose(df);
                fprintf(stderr, "[MON] wrote %s (%u MB from %08X)\n", dumps[dd].name,
                        dumps[dd].size >> 20, dumps[dd].va);
            }
        }
        if (n == 1 || n == 3 || n == 6) {
            static const uint32_t tex[] = { 0x80733000u, 0x80771000u, 0x80755000u };
            int t, k;
            for (t = 0; t < 3; t++) {
                uint32_t a = tex[t], nz = 0, nzf = 0;
                for (k = 0; k < 1024; k += 4) {
                    if (GMEM32(a + k)) nz++;
                    if (GMEM32(a - 0x80000000u + 0xF0000000u + k)) nzf++;
                }
                fprintf(stderr, "[MON]   tex %08X: %u/256 dwords non-zero (tiled alias %u/256):", a, nz, nzf);
                for (k = 0; k < 32; k += 4)
                    fprintf(stderr, " %08X", GMEM32(a + k));
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }
}

static void gh_monitor_start(void)
{
    static int started;
    if (!started++)
        CreateThread(NULL, 0, gh_monitor, NULL, 0, NULL);
}

/* ── Reached a placeholder for code the translator never produced ─────────
 * Called by every stub in recomp_stubs_unresolved.c. Each one is a branch of
 * the game whose code is being skipped, so the first hit of each is logged
 * with where it came from. */
void recomp_stub_hit(uint32_t va)
{
    enum { N = 512 };
    static uint32_t seen[N];
    static int count;
    int i;
    for (i = 0; i < count; i++)
        if (seen[i] == va)
            return;
    if (count < N)
        seen[count++] = va;
    fprintf(stderr, "[STUB] code at 0x%08X skipped (never translated); return to %08X, esp %08X\n",
            va, GMEM32(g_esp), g_esp);
}

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /* Rate-limited per address (first 3, then every power of two): a missing
     * function inside a per-frame loop logged ~1000 times a second with a
     * flush each, which alone cost Orta's stage 2 (in the Orta port) a third of its frame rate.
     * The first line per address is all the run loop needs to seed it. */
    enum { SLOTS = 64 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int nseen;
    uint64_t n = 0;
    int i;
    for (i = 0; i < nseen; i++)
        if (seen[i] == va) { n = ++hits[i]; break; }
    if (i == nseen && nseen < SLOTS) { seen[nseen] = va; hits[nseen] = n = 1; nseen++; }
    if (n > 3 && (n & (n - 1)))
        return;
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu, this one %llu times)\n",
            va, (unsigned long long)g_icall_count, (unsigned long long)n);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    /* First sighting: say WHERE. The caller pushed its guest return address
     * just before this call, so the guest stack names the call site and the
     * chain above it. */
    if (hits[i] == 1 && g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int k, shown = 0;
        fprintf(stderr, "  called from (guest return addresses, innermost first):");
        for (k = 0; k < 64 && shown < 8; k++) {
            uint32_t v = sp[k];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, " %08X", v);
                shown++;
            }
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}


/* Kick watchdog (with RECOMP_SYNC_KICK_LAG > 0). Letting the game run ahead
 * of the GPU once hung a room load: D3D queued fences without kicking them
 * and then waited on them, with the reader idle at PUT. If the reader sits
 * idle at PUT for RECOMP_KICK_WATCHDOG_MS (default 30) while D3D has
 * written commands past it, this publishes them -- what the next kick would
 * have done. Compare-and-swap, so a kick the game makes meanwhile wins and
 * PUT never moves backwards. RECOMP_KICK_WATCHDOG=0 turns it off. */
long _InterlockedCompareExchange(long volatile *, long, long);   /* an intrinsic on x64 */
#pragma intrinsic(_InterlockedCompareExchange)
static volatile uint32_t s_kick_dev;
static unsigned long __stdcall kick_watchdog(void *arg)
{
    const char *e = getenv("RECOMP_KICK_WATCHDOG_MS");
    unsigned long limit = e ? (unsigned long)atol(e) : 30ul, idle = 0;
    unsigned kicks = 0;
    (void)arg;
    if (limit < 5) limit = 5;
    for (;;) {
        uint32_t dev, user, put, get, push, want;
        Sleep(5);
        dev = s_kick_dev;
        if (!dev || (GMEM32(dev + 8) & 0x2000u) || GMEM32(0x00181960u))
            continue;
        user = GMEM32(dev + 0x1C10);
        if (!user)
            continue;
        put = GMEM32(user + 0x40);
        get = GMEM32(user + 0x44);
        push = (GMEM32(dev + 8) & 4u) ? GMEM32(dev + 0x76C) : GMEM32(dev);
        want = push & 0x0FFFFFFFu;
        if ((get & 0x0FFFFFFFu) != (put & 0x0FFFFFFFu) || want == (put & 0x0FFFFFFFu) || !want) {
            idle = 0;
            continue;
        }
        idle += 5;
        if (idle < limit)
            continue;
        idle = 0;
        if ((uint32_t)_InterlockedCompareExchange((volatile long *)&GMEM32(user + 0x40),
                                                 (long)want, (long)put) == put
            && kicks++ < 20) {
            fprintf(stderr, "[KICK] watchdog: reader idle at %08X with commands to %08X"
                            " -- kicked them\n", put, want);
            fflush(stderr);
        }
    }
    return 0;
}

/* ── D3D KickOff, synchronous (0x177280) ──────────────────────────────────
 *
 * thiscall(device), no arguments, plain ret. The original flushes the
 * write-combine buffer (PFB 0x100410 bit 16, acknowledged by the runtime),
 * publishes PUT, and returns at once: the GPU catches up in its own time.
 *
 * Here the GPU is a host thread, and D3D decides how much of its ring is
 * free from the GET it reads back. Grabbed by the Ghoulies' first level
 * load overwrote commands the reader had not reached yet -- the reader
 * found the next lap's data mid-method and lost its place, and the game
 * waited for ever on a fence it had skipped (ring dumps: the commands under
 * the reader changed while it sat there). Waiting here until the reader has
 * reached PUT keeps the ring exactly as D3D believes it to be.
 * RECOMP_SYNC_KICK=0 restores the asynchronous kick; the wait gives up
 * after RECOMP_SYNC_KICK_MS (default 250) so a stuck reader cannot freeze
 * the game on top. */
void sub_00177280(void)
{
    static int sync = -1;
    static unsigned long limit_ms;
    /* RECOMP_SYNC_KICK_LAG (bytes, default 0): how far the reader may trail
     * PUT before the kick waits. 0 is the full wait (the game and the GPU
     * never overlap); a few hundred KB of a 2 MB ring lets the game build
     * the next frame while the GPU draws this one, and the ring start D3D
     * wraps back to was still read long before. */
    static uint32_t max_lag;
    uint32_t dev = g_ecx;
    uint32_t flags = GMEM32(dev + 8);
    uint32_t push = (flags & 4u) ? GMEM32(dev + 0x76C) : GMEM32(dev);
    s_kick_dev = dev;

    if (sync < 0) {
        const char *e = getenv("RECOMP_SYNC_KICK");
        const char *m = getenv("RECOMP_SYNC_KICK_MS");
        const char *l = getenv("RECOMP_SYNC_KICK_LAG");
        sync = !(e && *e == '0');
        limit_ms = m ? (unsigned long)atol(m) : 250ul;
        max_lag = l ? (uint32_t)strtoul(l, NULL, 0) : 0u;
        fprintf(stderr, "[KICK] synchronous kick-off %s, reader may trail by %u bytes\n",
                sync ? "on" : "off", max_lag);
        if (sync && max_lag) {
            const char *w = getenv("RECOMP_KICK_WATCHDOG");
            if (!(w && *w == '0'))
                CreateThread(NULL, 0, kick_watchdog, NULL, 0, NULL);
        }
    }

    if (flags & 0x2000u) {
        /* D3D's GPU-less mode (never seen here): a fake register block,
         * and the fence marked reached straight away. */
        uint32_t fake = 0x00181950u - 0x40u, p = GMEM32(dev) & 0x0FFFFFFFu;
        GMEM32(dev + 0x1C10) = fake;
        GMEM32(fake + 0x40) = p;
        GMEM32(GMEM32(dev + 0x1C10) + 0x44) = p;
        GMEM32(GMEM32(dev + 0x30)) = GMEM32(dev + 0x2C) - 2u;
        GMEM32(dev + 0x1DD4) = GMEM32(dev + 0x2468);
        g_esp += 4;
        return;
    }
    {
        uint32_t regs = GMEM32(GMEM32(0x00181E08u) + 0x1C18u);
        uint32_t user = GMEM32(dev + 0x1C10);
        uint32_t want = push & 0x0FFFFFFFu;
        /* The write-combine flush D3D raises here (PFB 0x100410 bit 16)
         * guards real hardware's buffered writes; this process's writes are
         * already in order, so PUT goes straight out -- no handshake with
         * the ack thread. RECOMP_KICK_FLUSH=1 keeps the handshake. */
        {
            static int flush_ack = -1;
            if (flush_ack < 0) {
                const char *f = getenv("RECOMP_KICK_FLUSH");
                flush_ack = f && *f == '1';
            }
            if (flush_ack) {
                GMEM32(regs + 0x100410) = GMEM32(regs + 0x100410) | 0x10000u;
                while (GMEM32(regs + 0x100410) & 0x10000u)
                    ;                           /* write-combine flush ack */
            } else
                (void)regs;
        }
        GMEM32(user + 0x40) = want;             /* PUT */
        if (GMEM32(0x00181960u)) {
            while (GMEM32(user + 0x40) != GMEM32(user + 0x44))
                Sleep(0);
            GMEM32(dev + 8) = GMEM32(dev + 8) | 0x2000u;
        } else if (sync) {
            unsigned long t0 = GetTickCount();
            static unsigned timeouts;
            uint32_t lo = GMEM32(dev + 0x24) & 0x0FFFFFFFu, hi = GMEM32(dev + 0x28) & 0x0FFFFFFFu;
            for (;;) {
                uint32_t get = GMEM32(user + 0x44) & 0x0FFFFFFFu, lag;
                if (get == want)
                    break;
                if (max_lag && hi > lo && get >= lo && get < hi && want >= lo && want <= hi) {
                    lag = want >= get ? want - get : (want - lo) + (hi - get);
                    if (lag <= max_lag)
                        break;
                }
                if (GetTickCount() - t0 > limit_ms) {
                    if (timeouts++ < 20)
                        fprintf(stderr, "[KICK] reader still at %08X, PUT %08X after %lu ms"
                                        " -- not waiting longer\n",
                                GMEM32(user + 0x44), want, limit_ms);
                    break;
                }
                Sleep(0);
            }
        }
    }
    g_esp += 4;
}

/* ── Game speed at 60 fps (0x000FC030, once a frame) ─────────────────────
 *
 * The game's frame time is the number of vblanks its vblank callback
 * (0x000FC3B0) counted between two "swap done" reports, over 60 (50 on PAL),
 * capped at 5/60. With the flips forced to every vblank (RECOMP_FLIP_INTERVAL
 * =1), D3D still reports the swaps as if every other vblank, so each 60 fps
 * frame was given 1/30 s: the game ran at double speed (3 Nov). Here the
 * frame time is the real time since the last frame instead -- right at any
 * frame rate. RECOMP_REALTIME_DT=0 keeps the vblank count, as the game did.
 */
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *c);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *f);

void sub_000FC030(void)
{
    static int rt = -1;
    static long long last, freq;
    long long now = 0;
    uint32_t rate = (GMEM32(0x005096E0u) && !GMEM32(0x005096E4u)) ? 50u : 60u;
    float maxdt = 5.0f / (float)rate, dt, x;
    int whole;

    if (rt < 0) {
        const char *e = getenv("RECOMP_REALTIME_DT");
        rt = !(e && *e == '0');
        QueryPerformanceFrequency(&freq);
        fprintf(stderr, "[DT] frame time from %s\n", rt ? "real time" : "the vblank count");
        fflush(stderr);
    }
    QueryPerformanceCounter(&now);
    GMEM32(0x005096BCu) = (uint32_t)now - GMEM32(0x0050E0D8u);
    if (rt && last && freq > 0) {
        dt = (float)((double)(now - last) / (double)freq);
        if (dt < 0.0f) dt = 0.0f;
    } else {
        dt = (float)GMEM32(0x005096D0u) / (float)rate;
    }
    last = now;
    *(volatile float *)&GMEM32(0x005096B8u) = dt;
    if (dt > maxdt) dt = maxdt;
    *(volatile float *)&GMEM32(0x005096C0u) = dt;
    x = dt * 300.0f + *(volatile float *)&GMEM32(0x005096C8u);
    whole = (int)x;
    GMEM32(0x005096C4u) = (uint32_t)whole;
    *(volatile float *)&GMEM32(0x005096C8u) = x - (float)whole;
    g_eax = (uint32_t)whole;
}

/* ── Bink copy-to-buffer (0x001A7D00), with a memory snapshot ─────────────
 *
 * The same as the game's: re-push the arguments for 0x001A6F60 (which pops
 * eleven) and return popping seven. Written here only so that, while
 * bink_dump.flag holds "1", the 400th call saves the whole of memory just
 * before the copy and again just after it (binkdump_{low,contig}_{pre,post}
 * .bin, registers in binkdump.txt): the video decoder's artefacts are then
 * reproduced off-line by running the original and the translated code on the
 * same state and comparing (3 Nov). */
extern __declspec(thread) uint32_t g_edx;
extern __declspec(thread) uint32_t g_ebx, g_esi, g_edi, g_ebp;
extern void sub_001A6F60(void);

static void bink_snapshot(const char *when)
{
    static const struct { uint32_t va; const char *name; } part[] = {
        { 0x00000000u, "low" }, { 0x80000000u, "contig" },
    };
    int p;
    for (p = 0; p < 2; p++) {
        char name[64];
        FILE *df;
        uint32_t off;
        snprintf(name, sizeof name, "binkdump_%s_%s.bin", part[p].name, when);
        df = fopen(name, "wb");
        if (!df) continue;
        for (off = 0; off < 0x04000000u; off += 0x10000u) {
            uint32_t va = part[p].va + off;
            if (va < 0x10000u) {
                static uint8_t zeros[0x10000];
                fwrite(zeros, 1, sizeof zeros, df);
            } else
                fwrite((const void *)(g_xbox_mem_offset + (uintptr_t)va), 1, 0x10000u, df);
        }
        fclose(df);
    }
}

void sub_001A7D00(void)
{
    static int calls, armed = -1, done;
    uint32_t e = g_esp, a1, w, h;
    int snap = 0;

    if (armed < 0) {
        FILE *ff = fopen("bink_dump.flag", "rb");
        armed = 0;
        if (ff) { armed = fgetc(ff) == '1'; fclose(ff); }
    }
    if (armed && !done && ++calls == 400) {
        FILE *t = fopen("binkdump.txt", "w");
        snap = 1;
        done = 1;
        if (t) {
            int k;
            fprintf(t, "esp %08X ebx %08X esi %08X edi %08X ebp %08X eax %08X ecx %08X edx %08X\n",
                    g_esp, g_ebx, g_esi, g_edi, g_ebp, g_eax, g_ecx, g_edx);
            fprintf(t, "args");
            for (k = 0; k < 8; k++) fprintf(t, " %08X", GMEM32(e + 4u * k));
            fprintf(t, "\n");
            fclose(t);
        }
        bink_snapshot("pre");
    }

    a1 = GMEM32(e + 4u);
    w = GMEM32(a1 + 4u);
    h = GMEM32(a1);
#define BK_PUSH(v) do { g_esp -= 4u; GMEM32(g_esp) = (uint32_t)(v); } while (0)
    BK_PUSH(GMEM32(e + 0x1Cu));     /* flags */
    BK_PUSH(w);
    BK_PUSH(h);
    BK_PUSH(0);
    BK_PUSH(0);
    BK_PUSH(GMEM32(e + 0x18u));
    BK_PUSH(GMEM32(e + 0x14u));
    BK_PUSH(GMEM32(e + 0x10u));
    BK_PUSH(GMEM32(e + 0x0Cu));
    BK_PUSH(GMEM32(e + 0x08u));
    BK_PUSH(a1);
    BK_PUSH(0x001A7D33u);
#undef BK_PUSH
    g_eax = a1;
    g_ecx = GMEM32(e + 0x08u);
    g_edx = GMEM32(e + 0x0Cu);
    sub_001A6F60();
    if (snap) {
        FILE *t = fopen("binkdump.txt", "a");
        if (t) { fprintf(t, "after: esp %08X eax %08X\n", g_esp, g_eax); fclose(t); }
        bink_snapshot("post");
        fprintf(stderr, "[BINK] memory saved before and after copy call %d\n", calls);
        fflush(stderr);
    }
    g_esp += 4u + 0x1Cu;            /* ret 0x1C */
}

/* ── Guard: a corrupt contact list (0x0004D370) ──────────────────────────
 *
 * Walks a packed list of records (record += size table[type] until type 0)
 * for an object's entries, called per entry from 0x0004E1E0. Twice, in long
 * sessions, the entry array held garbage (0xCB9C0381, 0xB7E3901D -- float
 * data where pointers belong) and the walk read outside memory: a crash in
 * the same place both times (3 Nov). What overwrites the array is not known
 * yet. Until it is, a list that starts or leads outside guest memory is
 * skipped, and the first few are logged. This is the lifted code with those
 * checks; RECOMP_LIST_GUARD=0 turns the checks off. */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_funcs.h"   /* path from src/: MSVC has no gen include dir */

static int gh_va_ok(uint32_t va)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_LIST_GUARD");
        on = !(e && *e == '0');
    }
    if (!on)
        return 1;
    return (va >= 0x00010000u && va < 0x04000000u - 16u)
        || (va >= 0x80000000u && va < 0x84000000u - 16u);
}

static void gh_list_bad(const char *what, uint32_t va)
{
    static int n;
    if (n++ < 10) {
        fprintf(stderr, "[GUARD] contact list: %s at %08X is outside memory -- skipped\n",
                what, va);
        fflush(stderr);
    }
}

static int gh_list_ok(uint32_t node, uint32_t a1, uint32_t a2)
{
    (void)a1; (void)a2;
    if (gh_va_ok(node))
        return 1;
    gh_list_bad("list", node);
    return 0;
}

void sub_0004D370(void)
{
    uint32_t ebp = 0;
    ebp = g_ebp;  /* frameless: caller's frame */
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
    ebp = g_seh_ebp; /* fpo_leaf: inherit caller's frame */

    if (!gh_list_ok(eax, MEM32(esp + 0x14), MEM32(esp + 0x18))) {
        esp += 4; return;                 /* skip this list: ret, nothing pushed */
    }

loc_0004D370: ;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    esi = eax;
    _fa = (uint32_t)(MEM16(esi + 0xC)) & 0xFFFFu; _fb = (uint32_t)(0) & 0xFFFFu;
    _fas = (int32_t)(int16_t)(_fa); _fbs = (int32_t)(int16_t)(_fb); /* cmp MEM16(esi + 0xC), 0 (16-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0004D3D3; /* je: equal / zero */

loc_0004D37B: ;
    PUSH32(esp, ebx);
    ebx = MEM32(esp + 0x20);
    PUSH32(esp, ebp);
    ebp = MEM32(esp + 0x20);
    PUSH32(esp, edi);

loc_0004D386: ;
    eax = MEM32(esp + 0x30);
    ecx = MEM32(esp + 0x2C);
    PUSH32(esp, eax);
    PUSH32(esp, ebx);
    PUSH32(esp, ebp);
    edi = esp + 0x1C;
    PUSH32(esp, 0x0004D39Au); RECOMP_ABI_CALL(0x0004C6C0u, sub_0004C6C0); /* call 0x0004C6C0 */

loc_0004D39A: ;
    _fb = (uint32_t)(0xC) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    esp = esp + 0xC;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0004D3BE; /* je: equal / zero */

loc_0004D3A1: ;
    ecx = MEM32(esp + 0x10);
    edx = MEM32(esp + 0x20);
    eax = MEM32(esp + 0x1C);
    PUSH32(esp, ecx);
    ecx = MEM32(esp + 0x1C);
    PUSH32(esp, edx);
    PUSH32(esp, esi);
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    PUSH32(esp, 0x0004D3BBu); RECOMP_ABI_CALL(0x0004C850u, sub_0004C850); /* call 0x0004C850 */

loc_0004D3BB: ;
    _fb = (uint32_t)(0x14) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    esp = esp + 0x14;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */

loc_0004D3BE: ;
    edx = ZX16(MEM16(esi + 0xC));
    _fb = (uint32_t)(MEM32(edx * 4 + 0x423630)) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    esi = esi + MEM32(edx * 4 + 0x423630);
    _fa = (uint32_t)(esi) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */
    if (!gh_va_ok(esi)) { gh_list_bad("next record", esi); goto loc_0004D3D0; }
    _fa = (uint32_t)(MEM16(esi + 0xC)) & 0xFFFFu; _fb = (uint32_t)(0) & 0xFFFFu;
    _fas = (int32_t)(int16_t)(_fa); _fbs = (int32_t)(int16_t)(_fb); /* cmp MEM16(esi + 0xC), 0 (16-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_0004D386; /* jne: not equal / not zero */

loc_0004D3D0: ;
    POP32(esp, edi);
    POP32(esp, ebp);
    POP32(esp, ebx);

loc_0004D3D3: ;
    POP32(esp, esi);
    POP32(esp, ecx);
    esp += 4; return; /* ret */

}

