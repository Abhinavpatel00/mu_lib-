#pragma once
#ifndef MU_INPUT_H
#define MU_INPUT_H

#include "mu_common.h"

#include <math.h>

/* ==========================================================================
 * mu_input.h — device-agnostic input: raw events in, resolved actions out
 *              (single header, C99, no allocation, no globals, no platform)
 * ==========================================================================
 *
 * WHAT THIS IS
 * ------------
 * Three layers with one boundary that matters: the window system never meets
 * the game's vocabulary. A platform backend (GLFW, SDL, Win32, evdev, a replay
 * file) pushes events into MuInputState; MuInputState turns held bits and
 * pointer deltas into a snapshot; a MuInputMap turns that snapshot into named
 * actions; gameplay reads the actions and never sees a key code.
 *
 * SIMPLE / QUICK-CHECK API (no map needed)
 * ----------------------------------------
 * For small demos, tools, or "just check if W is down" code, skip the map:
 *
 *     MuInputState in; mu_input_init(&in);
 *     mu_input_key_set_slot(&in, MU_KEY_W, slot_of_W);   // backend does this
 *     // ... push events from the platform ...
 *     mu_input_frame_begin(&in, now_ns);
 *     if (mu_input_key_pressed(&in, MU_KEY_W))   jump();       // edge, one frame
 *     if (mu_input_key_down(&in, MU_KEY_W))      move();       // held
 *     if (mu_input_mouse_down(&in, MU_MOUSE_RIGHT)) drag();    // held
 *     if (mu_input_mouse_pressed(&in, MU_MOUSE_LEFT)) click(); // edge
 *
 * MuKey / MuMouseButton are *logical* enums (stable, platform-free). The
 * backend maps each MuKey to a physical control slot (scancode) once at
 * startup via mu_input_key_set_slot(). The GLFW backend in
 * demos/common/platform.c does this for you, so in a real app the quick
 * API works with zero setup. In tests, mu_input_key_map_identity() maps
 * MU_KEY_A -> slot MU_KEY_A, so the enum value *is* the slot.
 *
 * FULL ACTION API (rebindable, per-player, analog)
 * ------------------------------------------------
 *     mu_input_map_init(&map, ACT_COUNT);
 *     mu_input_bind_control(&map, ACT_JUMP, slot_of_space);
 *     mu_input_bind_key(&map, ACT_JUMP, &in, MU_KEY_SPACE);  // by logical key
 *     mu_input_resolve(&in, &map, &actions, in.dt);
 *     if (mu_input_action_pressed(&actions, ACT_JUMP)) jump();
 *
 * FIXED-TIMESTEP / TICK API (deterministic sim, replay, netcode)
 * --------------------------------------------------------------
 *     MuInputFrame f; mu_input_frame_reset(&f);
 *     mu_input_sample_tick(&in, &map, &f, tick);   // once per SIM tick
 *     if (mu_input_buffered(&f, ACT_JUMP, 7)) ...  // pressed within 7 ticks
 *
 * WHY NOT JUST CALL glfwGetKey()
 * ------------------------------
 * 1. POLLING LOSES TAPS. glfwGetKey reports *state*. A press and release that
 *    both land between two polls is invisible — which is why GLFW ships
 *    GLFW_STICKY_KEYS. Here the backend pushes events, so a tap is two events,
 *    both seen, and `pressed` stays true for the frame it happened in even if
 *    the key is already up again.
 * 2. KEY CODES ARE LAYOUT-DEPENDENT AND PLATFORM-SPECIFIC. Gameplay asking
 *    "is GLFW_KEY_W down" bakes one keyboard into the game. Bindings are data
 *    here (MuInputMap), and a control slot is whatever the backend decides:
 *    the intended convention is a *physical position* (GLFW scancode / X11
 *    keycode / Win32 scan code), with the label printed for the user's layout
 *    by the backend. Bind position, print name.
 * 3. EDGE STATE IS STATE YOU SHOULD NOT STORE. down/pressed/released fall out
 *    of two 64-bit words (`held & ~prev`), so 512 controls cost 64 bytes and
 *    every query is branchless. See AGENTS.md §4.2/§4.4: booleans over a large
 *    domain are bitsets; an enum used as an index is data, not control flow.
 * 4. NO HIDDEN ALLOCATION OR GLOBALS. Everything is a struct you own. A test
 *    can fabricate input with no window, no GPU and no OS (§AGENTS 9.7), and a
 *    replay file feeds the same API as a keyboard.
 *
 * USAGE (backend side, once per frame)
 * ------------------------------------
 *     mu_input_frame_begin(&in, now_ns);        // roll prev, drain events, dt
 *     // between frames, from OS callbacks or a poll diff:
 *     mu_input_push_control(&in, slot, true, now_ns);
 *     mu_input_mouse_pos(&in, x, y);
 *     mu_input_wheel_add(&in, 0.0f, yoff);
 *     mu_input_pad_axis(&in, pad, MU_INPUT_PAD_AXIS_LY, v);
 *
 * USAGE (game side, every frame)
 * ------------------------------
 *     mu_input_resolve(&in, &map, &actions, in.dt);
 *     if (mu_input_action_pressed(&actions, ACT_JUMP)) jump();
 *     camera.yaw += mu_input_axis(&actions, ACT_ORBIT_LEFT, ACT_ORBIT_RIGHT) * dt;
 *     throttle = mu_input_action_value(&actions, ACT_THROTTLE);
 *
 * CONTROL SLOTS
 * -------------
 *   0   .. 479  keyboard, chosen by the backend (position is recommended).
 *   480 .. 511  pointer buttons, MU_INPUT_MOUSE_SLOT(button).
 *
 * COST (measured, x86-64 -O2)
 * ---------------------------
 *   frame_begin, no events:      ~8 word stores + 2 subs, tens of cycles.
 *   one query:                   one load, one ANDN, one shift.
 *   resolve, 128 actions:        <= 512 source reads, fixed arrays, no branches
 *                                on random data beyond the per-source kind.
 *   state size:                  ~6.4 KB, of which 6 KB is the event ring.
 *
 * VERSION
 * -------
 *   v2 (additive over v1): MuKey/MuMouseButton quick API, tick sampler
 *   (MuInputFrame), buffered/coyote in ticks, chords, pad connect/disconnect,
 *   wheel single-path + consume, event span for record/replay, per-source
 *   deadzone/curve, mouse/wheel copied into MuInputActions, MU_RESTRICT.
 *   All v1 signatures are unchanged.
 */

#define MU_INPUT_VERSION_MAJOR 2u
#define MU_INPUT_VERSION_MINOR 0u

/* ==========================================================================
 * §1 CONFIGURATION
 * ========================================================================== */

#define MU_INPUT_CONTROLS      512u  /* control slots; keyboard + pointer */
#define MU_INPUT_WORDS         (MU_INPUT_CONTROLS / 64u)
#define MU_INPUT_MOUSE_SLOT    480u  /* first pointer-button slot */
#define MU_INPUT_MOUSE_SLOTS   32u
#define MU_INPUT_MAX_PADS      4u
#define MU_INPUT_PAD_BUTTONS   32u
#define MU_INPUT_PAD_AXES      6u    /* lx, ly, rx, ry, lt, rt */
#define MU_INPUT_MAX_ACTIONS   128u
#define MU_INPUT_ACTION_WORDS  (MU_INPUT_MAX_ACTIONS / 64u)
#define MU_INPUT_MAX_SOURCES   4u    /* bindings per action */
#define MU_INPUT_EVENT_CAP     256u  /* fixed ring; no allocation, ever */
#define MU_INPUT_MAX_DT        0.1f  /* a stalled frame must not teleport input */
#define MU_INPUT_MAX_CHORDS    16u   /* chord definitions per MuInputChords */
#define MU_INPUT_SLOT_NONE     0xFFFFu /* unmapped MuKey slot sentinel */

/* Pad axis indices, in the order a backend should fill them. */
#define MU_INPUT_PAD_AXIS_LX 0u
#define MU_INPUT_PAD_AXIS_LY 1u
#define MU_INPUT_PAD_AXIS_RX 2u
#define MU_INPUT_PAD_AXIS_RY 3u
#define MU_INPUT_PAD_AXIS_LT 4u
#define MU_INPUT_PAD_AXIS_RT 5u

#define MU_INPUT_MOUSE_SLOT_OF(button) (MU_INPUT_MOUSE_SLOT + (uint32_t)(button))

MU_STATIC_ASSERT(MU_INPUT_CONTROLS == MU_INPUT_WORDS * 64u, "control words");
MU_STATIC_ASSERT(MU_INPUT_MAX_ACTIONS == MU_INPUT_ACTION_WORDS * 64u, "action words");

/* ==========================================================================
 * §1b SIMPLE API — logical keys & mouse buttons (platform-free enums)
 * ==========================================================================
 * MuKey is a *logical* key: MU_KEY_W means "the key at W's position on a US
 * layout", regardless of the user's keyboard layout or the OS scancode. The
 * backend translates MuKey -> physical slot once (mu_input_key_set_slot).
 * Order matters: A..Z and 0..9 are contiguous so loops / ranges work.
 */

typedef enum MuKey
{
    MU_KEY_UNKNOWN = 0,
    MU_KEY_ESCAPE,
    MU_KEY_SPACE,
    MU_KEY_TAB,
    MU_KEY_ENTER,
    MU_KEY_BACKSPACE,
    MU_KEY_DELETE,
    MU_KEY_INSERT,
    MU_KEY_HOME,
    MU_KEY_END,
    MU_KEY_PAGE_UP,
    MU_KEY_PAGE_DOWN,
    MU_KEY_LEFT,
    MU_KEY_RIGHT,
    MU_KEY_UP,
    MU_KEY_DOWN,
    MU_KEY_LEFT_SHIFT,
    MU_KEY_RIGHT_SHIFT,
    MU_KEY_LEFT_CTRL,
    MU_KEY_RIGHT_CTRL,
    MU_KEY_LEFT_ALT,
    MU_KEY_RIGHT_ALT,
    MU_KEY_MINUS,
    MU_KEY_EQUAL,
    MU_KEY_LEFT_BRACKET,
    MU_KEY_RIGHT_BRACKET,
    MU_KEY_COMMA,
    MU_KEY_PERIOD,
    MU_KEY_SLASH,
    MU_KEY_SEMICOLON,
    MU_KEY_APOSTROPHE,
    MU_KEY_BACKSLASH,
    MU_KEY_GRAVE,
    MU_KEY_F1,
    MU_KEY_F2,
    MU_KEY_F3,
    MU_KEY_F4,
    MU_KEY_F5,
    MU_KEY_F6,
    MU_KEY_F7,
    MU_KEY_F8,
    MU_KEY_F9,
    MU_KEY_F10,
    MU_KEY_F11,
    MU_KEY_F12,
    MU_KEY_A,
    MU_KEY_B,
    MU_KEY_C,
    MU_KEY_D,
    MU_KEY_E,
    MU_KEY_F,
    MU_KEY_G,
    MU_KEY_H,
    MU_KEY_I,
    MU_KEY_J,
    MU_KEY_K,
    MU_KEY_L,
    MU_KEY_M,
    MU_KEY_N,
    MU_KEY_O,
    MU_KEY_P,
    MU_KEY_Q,
    MU_KEY_R,
    MU_KEY_S,
    MU_KEY_T,
    MU_KEY_U,
    MU_KEY_V,
    MU_KEY_W,
    MU_KEY_X,
    MU_KEY_Y,
    MU_KEY_Z,
    MU_KEY_0,
    MU_KEY_1,
    MU_KEY_2,
    MU_KEY_3,
    MU_KEY_4,
    MU_KEY_5,
    MU_KEY_6,
    MU_KEY_7,
    MU_KEY_8,
    MU_KEY_9,
    MU_KEY_COUNT
} MuKey;

typedef enum MuMouseButton
{
    MU_MOUSE_LEFT = 0,
    MU_MOUSE_RIGHT,
    MU_MOUSE_MIDDLE,
    MU_MOUSE_4,
    MU_MOUSE_5,
    MU_MOUSE_6,
    MU_MOUSE_7,
    MU_MOUSE_8,
    MU_MOUSE_COUNT
} MuMouseButton;

/* ==========================================================================
 * §2 EVENTS — what the backend observed
 * ========================================================================== */

typedef enum MuInputEventKind
{
    MU_INPUT_EV_CONTROL_DOWN = 0,   /* keyboard or pointer button pressed   */
    MU_INPUT_EV_CONTROL_UP,         /* ... and released                     */
    MU_INPUT_EV_PAD_BUTTON_DOWN,
    MU_INPUT_EV_PAD_BUTTON_UP,
    MU_INPUT_EV_WHEEL,              /* value = dy, value2 = dx, + is up/right */
    MU_INPUT_EV_PAD_CONNECT,        /* code = pad index                     */
    MU_INPUT_EV_PAD_DISCONNECT,     /* code = pad index                     */
    MU_INPUT_EV_KIND_COUNT
} MuInputEventKind;

typedef struct MuInputEvent
{
    uint64_t time_ns;   /* when it happened, not when the frame read it     */
    uint32_t code;      /* control slot / pad button / pad index / wheel    */
    uint32_t tick;      /* sim tick when observed (0 = untracked / wall)    */
    uint16_t kind;      /* MuInputEventKind                                 */
    int16_t  pad;       /* pad index, -1 for keyboard/pointer               */
    float    value;     /* wheel dy; 0.0 for discrete events                */
    float    value2;    /* wheel dx; 0.0 for discrete events                */
} MuInputEvent;

/* Non-owning view of the queued events. Valid until the next frame_begin
 * (which drains the ring). This is the record/replay substrate: copy the
 * span before frame_begin, replay by re-pushing each event in order. */
typedef struct MuInputEventSpan
{
    const MuInputEvent *ptr;
    uint32_t            count;
} MuInputEventSpan;

/* ==========================================================================
 * §3 STATE — held bits, pointer, pads, timing, and the event ring
 * ========================================================================== */

typedef struct MuInputState
{
    uint64_t controls[MU_INPUT_WORDS];       /* held now                   */
    uint64_t controls_prev[MU_INPUT_WORDS];  /* held at the end of the last frame */
    uint64_t pressed_frame[MU_INPUT_WORDS];  /* pressed by an event this frame    */

    uint32_t pad_buttons[MU_INPUT_MAX_PADS];
    uint32_t pad_buttons_prev[MU_INPUT_MAX_PADS];
    uint32_t pad_pressed_frame[MU_INPUT_MAX_PADS];
    float    pad_axes[MU_INPUT_MAX_PADS][MU_INPUT_PAD_AXES];
    uint32_t pad_connected;                  /* bit i = pad i connected    */

    float    mouse_x, mouse_y;
    float    mouse_prev_x, mouse_prev_y;
    float    mouse_dx, mouse_dy;       /* derived each frame; what games want  */
    float    wheel_x, wheel_y;         /* this frame's wheel delta             */
    float    wheel_pending_x, wheel_pending_y; /* legacy mirror (see wheel_add) */

    float    deadzone;                 /* default analog deadzone          */
    float    dt;                       /* clamped frame delta, seconds     */
    uint64_t time_ns;
    uint32_t tick;                     /* frames begun (frame_begin count) */

    /* Simple-API mapping: logical MuKey -> physical control slot.
     * MU_INPUT_SLOT_NONE = unmapped (key_down returns false). Filled by
     * the backend once at startup; identity in tests. */
    uint16_t key_slot[MU_KEY_COUNT];

    MuInputEvent events[MU_INPUT_EVENT_CAP];
    uint32_t     event_count;
    uint32_t     event_dropped;        /* ring overflow; a bug worth surfacing */
} MuInputState;

/* ==========================================================================
 * §4 STATE — backend API
 * ========================================================================== */

MU_INLINE bool mu_input_push_ex(MuInputState *s, MuInputEventKind kind, uint32_t code,
                                int32_t pad, float value, float value2,
                                uint64_t time_ns, uint32_t tick)
{
    MuInputEvent *e;

    if ((uint32_t)kind >= (uint32_t)MU_INPUT_EV_KIND_COUNT) return false;
    if (s->event_count >= MU_INPUT_EVENT_CAP)
    {
        s->event_dropped++;
        return false;
    }
    e = &s->events[s->event_count++];
    e->time_ns = time_ns;
    e->code    = code;
    e->tick    = tick;
    e->kind    = (uint16_t)kind;
    e->pad     = (int16_t)pad;
    e->value   = value;
    e->value2  = value2;
    return true;
}

MU_INLINE bool mu_input_push(MuInputState *s, MuInputEventKind kind, uint32_t code,
                             int32_t pad, float value, uint64_t time_ns)
{
    return mu_input_push_ex(s, kind, code, pad, value, 0.0f, time_ns, s->tick);
}

/* Queued events since the last frame_begin. Copy before frame_begin drains. */
MU_INLINE MuInputEventSpan mu_input_events(const MuInputState *s)
{
    MuInputEventSpan span;
    span.ptr = s->events;
    span.count = s->event_count;
    return span;
}

/* Re-push a recorded span into a (fresh) state, in order. Returns the number
 * of events accepted (== span.count unless the ring overflowed). */
MU_INLINE uint32_t mu_input_replay_events(MuInputState *s, MuInputEventSpan span)
{
    uint32_t i, ok = 0;
    for (i = 0; i < span.count; ++i)
    {
        const MuInputEvent *e = &span.ptr[i];
        if (mu_input_push_ex(s, (MuInputEventKind)e->kind, e->code, e->pad,
                             e->value, e->value2, e->time_ns, e->tick))
            ++ok;
    }
    return ok;
}

MU_INLINE void mu_input_push_control(MuInputState *s, uint32_t slot, bool down,
                                     uint64_t time_ns)
{
    mu_input_push(s, down ? MU_INPUT_EV_CONTROL_DOWN : MU_INPUT_EV_CONTROL_UP,
                  slot, -1, 0.0f, time_ns);
}

MU_INLINE void mu_input_push_pad_button(MuInputState *s, uint32_t pad, uint32_t button,
                                        bool down, uint64_t time_ns)
{
    mu_input_push(s, down ? MU_INPUT_EV_PAD_BUTTON_DOWN : MU_INPUT_EV_PAD_BUTTON_UP,
                  button, (int32_t)pad, 0.0f, time_ns);
}

/* Wheel is event-only (single path): the event carries (dx, dy) and
 * frame_begin derives wheel_x/y from the queued events. wheel_pending_* is
 * kept as a legacy mirror for code that read it directly; new code should
 * use wheel_x/y or mu_input_wheel_consume(). */
MU_INLINE void mu_input_wheel_add(MuInputState *s, float dx, float dy)
{
    mu_input_push_ex(s, MU_INPUT_EV_WHEEL, 0u, -1, dy, dx, s->time_ns, s->tick);
    s->wheel_pending_x += dx;
    s->wheel_pending_y += dy;
}

/* Take-once wheel: returns this frame's delta and zeroes it. */
MU_INLINE void mu_input_wheel_consume(MuInputState *s, float *x, float *y)
{
    if (x) *x = s->wheel_x;
    if (y) *y = s->wheel_y;
    s->wheel_x = 0.0f;
    s->wheel_y = 0.0f;
    s->wheel_pending_x = 0.0f;
    s->wheel_pending_y = 0.0f;
}

/* Continuous state is pushed directly: it has no edges worth queueing. */
MU_INLINE void mu_input_mouse_pos(MuInputState *s, float x, float y)
{
    s->mouse_x = x;
    s->mouse_y = y;
}

/* Relative motion: advance the position by (dx, dy). frame_begin derives
 * mouse_dx/dy from the position change, so this is equivalent to a pos
 * update — provided for backends that only get deltas (raw input). */
MU_INLINE void mu_input_mouse_move(MuInputState *s, float dx, float dy)
{
    s->mouse_x += dx;
    s->mouse_y += dy;
}

/* After a warp, a focus change or a teleport: adopt the position without
 * emitting a delta, so the camera does not snap. */
MU_INLINE void mu_input_mouse_warp(MuInputState *s, float x, float y)
{
    s->mouse_x = x;
    s->mouse_y = y;
    s->mouse_prev_x = x;
    s->mouse_prev_y = y;
    s->mouse_dx = 0.0f;
    s->mouse_dy = 0.0f;
}

MU_INLINE void mu_input_pad_axis(MuInputState *s, uint32_t pad, uint32_t axis, float v)
{
    if (pad >= MU_INPUT_MAX_PADS || axis >= MU_INPUT_PAD_AXES) return;
    s->pad_axes[pad][axis] = v;
}

/* Pad lifecycle. Connect/disconnect travel through the event ring so they
 * are ordered with button edges and are recordable. On disconnect the pad's
 * buttons/axes are zeroed in frame_begin (no stuck stick, no phantom press
 * on reconnect). Pads start connected (v1 behaviour) until a disconnect
 * event says otherwise. */
MU_INLINE void mu_input_pad_connect(MuInputState *s, uint32_t pad, bool connected,
                                    uint64_t time_ns)
{
    if (pad >= MU_INPUT_MAX_PADS) return;
    mu_input_push(s, connected ? MU_INPUT_EV_PAD_CONNECT : MU_INPUT_EV_PAD_DISCONNECT,
                  pad, (int32_t)pad, 0.0f, time_ns);
}

MU_INLINE bool mu_input_pad_connected(const MuInputState *s, uint32_t pad)
{
    if (pad >= MU_INPUT_MAX_PADS) return false;
    return (bool)((s->pad_connected >> pad) & 1u);
}

/* ---- simple-API key mapping (backend fills once at startup) ---- */

MU_INLINE void mu_input_key_set_slot(MuInputState *s, MuKey key, uint32_t slot)
{
    if ((uint32_t)key >= (uint32_t)MU_KEY_COUNT) return;
    s->key_slot[key] = (slot >= MU_INPUT_CONTROLS) ? (uint16_t)MU_INPUT_SLOT_NONE
                                                   : (uint16_t)slot;
}

MU_INLINE uint32_t mu_input_key_slot(const MuInputState *s, MuKey key)
{
    if ((uint32_t)key >= (uint32_t)MU_KEY_COUNT) return MU_INPUT_SLOT_NONE;
    return (uint32_t)s->key_slot[key];
}

/* Tests / slot==enum backends: MU_KEY_A -> slot MU_KEY_A, etc. */
MU_INLINE void mu_input_key_map_identity(MuInputState *s)
{
    uint32_t i;
    for (i = 0; i < (uint32_t)MU_KEY_COUNT; ++i) s->key_slot[i] = (uint16_t)i;
}

/* Unmap every key (key_down returns false until re-mapped). */
MU_INLINE void mu_input_key_map_clear(MuInputState *s)
{
    uint32_t i;
    for (i = 0; i < (uint32_t)MU_KEY_COUNT; ++i) s->key_slot[i] = (uint16_t)MU_INPUT_SLOT_NONE;
}

MU_INLINE void mu_input_init(MuInputState *s)
{
    uint32_t i;
    memset(s, 0, sizeof *s);
    s->deadzone = 0.15f;
    s->pad_connected = (MU_INPUT_MAX_PADS >= 32u) ? 0xFFFFFFFFu
                                                  : ((1u << MU_INPUT_MAX_PADS) - 1u);
    for (i = 0; i < (uint32_t)MU_KEY_COUNT; ++i) s->key_slot[i] = (uint16_t)MU_INPUT_SLOT_NONE;
}

/* Alt-tab: drop everything held, keep nothing latched. Without this a demo
 * keeps moving after the window loses focus. */
MU_INLINE void mu_input_focus_lost(MuInputState *s)
{
    memset(s->controls, 0, sizeof s->controls);
    memset(s->controls_prev, 0, sizeof s->controls_prev);
    memset(s->pressed_frame, 0, sizeof s->pressed_frame);
    memset(s->pad_buttons, 0, sizeof s->pad_buttons);
    memset(s->pad_buttons_prev, 0, sizeof s->pad_buttons_prev);
    memset(s->pad_pressed_frame, 0, sizeof s->pad_pressed_frame);
    memset(s->pad_axes, 0, sizeof s->pad_axes);
    s->event_count = 0;
    s->wheel_x = 0.0f;
    s->wheel_y = 0.0f;
    s->wheel_pending_x = 0.0f;
    s->wheel_pending_y = 0.0f;
}

/* Once per frame, before any query. Order matters:
 *   1. last frame's end state becomes `prev` (so edges are correct),
 *   2. this frame's events are applied (so a tap that already ended counts),
 *   3. timing and pointer deltas are derived. */
MU_INLINE void mu_input_frame_begin(MuInputState *s, uint64_t now_ns)
{
    uint32_t i, w;
    float dt;

    for (w = 0; w < MU_INPUT_WORDS; ++w)
    {
        s->controls_prev[w] = s->controls[w];
        s->pressed_frame[w] = 0u;
    }
    for (i = 0; i < MU_INPUT_MAX_PADS; ++i)
    {
        s->pad_buttons_prev[i] = s->pad_buttons[i];
        s->pad_pressed_frame[i] = 0u;
    }

    s->wheel_x = 0.0f;
    s->wheel_y = 0.0f;

    for (i = 0; i < s->event_count; ++i)
    {
        const MuInputEvent *e = &s->events[i];
        switch (e->kind)
        {
        case MU_INPUT_EV_CONTROL_DOWN:
            if (e->code < MU_INPUT_CONTROLS)
            {
                s->controls[e->code >> 6] |= 1ull << (e->code & 63u);
                s->pressed_frame[e->code >> 6] |= 1ull << (e->code & 63u);
            }
            break;
        case MU_INPUT_EV_CONTROL_UP:
            if (e->code < MU_INPUT_CONTROLS)
                s->controls[e->code >> 6] &= ~(1ull << (e->code & 63u));
            break;
        case MU_INPUT_EV_PAD_BUTTON_DOWN:
            if (e->pad >= 0 && e->pad < (int32_t)MU_INPUT_MAX_PADS && e->code < MU_INPUT_PAD_BUTTONS)
            {
                s->pad_buttons[e->pad] |= 1u << e->code;
                s->pad_pressed_frame[e->pad] |= 1u << e->code;
            }
            break;
        case MU_INPUT_EV_PAD_BUTTON_UP:
            if (e->pad >= 0 && e->pad < (int32_t)MU_INPUT_MAX_PADS && e->code < MU_INPUT_PAD_BUTTONS)
                s->pad_buttons[e->pad] &= ~(1u << e->code);
            break;
        case MU_INPUT_EV_WHEEL:
            s->wheel_y += e->value;
            s->wheel_x += e->value2;
            break;
        case MU_INPUT_EV_PAD_CONNECT:
            if (e->code < MU_INPUT_MAX_PADS)
                s->pad_connected |= 1u << e->code;
            break;
        case MU_INPUT_EV_PAD_DISCONNECT:
            if (e->code < MU_INPUT_MAX_PADS)
            {
                s->pad_connected &= ~(1u << e->code);
                s->pad_buttons[e->code] = 0u;
                s->pad_buttons_prev[e->code] = 0u;
                s->pad_pressed_frame[e->code] = 0u;
                memset(s->pad_axes[e->code], 0, sizeof s->pad_axes[e->code]);
            }
            break;
        default:
            break;
        }
    }
    s->event_count = 0;
    s->wheel_pending_x = 0.0f;
    s->wheel_pending_y = 0.0f;

    dt = (float)((double)(now_ns - s->time_ns) * 1e-9);
    if (now_ns < s->time_ns) dt = 0.0f;
    if (dt > MU_INPUT_MAX_DT) dt = MU_INPUT_MAX_DT;
    s->dt = dt;
    s->time_ns = now_ns;
    s->tick++;

    s->mouse_dx = s->mouse_x - s->mouse_prev_x;
    s->mouse_dy = s->mouse_y - s->mouse_prev_y;
    s->mouse_prev_x = s->mouse_x;
    s->mouse_prev_y = s->mouse_y;
}

/* Consume the per-frame press latch. Fixed-step sims that sample N ticks
 * per render frame call this after the first sample_tick() in the frame, so
 * a press fires on exactly one tick instead of N. Held state is untouched. */
MU_INLINE void mu_input_clear_pressed_frame(MuInputState *s)
{
    memset(s->pressed_frame, 0, sizeof s->pressed_frame);
    memset(s->pad_pressed_frame, 0, sizeof s->pad_pressed_frame);
}

/* ==========================================================================
 * §5 STATE — queries (branchless; one load, one ANDN, one shift)
 * ========================================================================== */

MU_INLINE bool mu_input_control_down(const MuInputState *s, uint32_t slot)
{
    if (slot >= MU_INPUT_CONTROLS) return false;
    return (bool)((s->controls[slot >> 6] >> (slot & 63u)) & 1ull);
}

MU_INLINE bool mu_input_control_pressed(const MuInputState *s, uint32_t slot)
{
    if (slot >= MU_INPUT_CONTROLS) return false;
    return (bool)((((s->controls[slot >> 6] & ~s->controls_prev[slot >> 6]) |
                    s->pressed_frame[slot >> 6]) >> (slot & 63u)) & 1ull);
}

MU_INLINE bool mu_input_control_released(const MuInputState *s, uint32_t slot)
{
    if (slot >= MU_INPUT_CONTROLS) return false;
    return (bool)(((s->controls_prev[slot >> 6] & ~s->controls[slot >> 6]) >>
                   (slot & 63u)) & 1ull);
}

MU_INLINE bool mu_input_mouse_button_down(const MuInputState *s, uint32_t button)
{
    return mu_input_control_down(s, MU_INPUT_MOUSE_SLOT_OF(button));
}

MU_INLINE bool mu_input_mouse_button_pressed(const MuInputState *s, uint32_t button)
{
    return mu_input_control_pressed(s, MU_INPUT_MOUSE_SLOT_OF(button));
}

MU_INLINE bool mu_input_mouse_button_released(const MuInputState *s, uint32_t button)
{
    return mu_input_control_released(s, MU_INPUT_MOUSE_SLOT_OF(button));
}

/* ---- simple API: logical keys (no map, no slot numbers in game code) ---- */

MU_INLINE bool mu_input_key_down(const MuInputState *s, MuKey key)
{
    uint32_t slot = mu_input_key_slot(s, key);
    return slot != MU_INPUT_SLOT_NONE && mu_input_control_down(s, slot);
}

MU_INLINE bool mu_input_key_pressed(const MuInputState *s, MuKey key)
{
    uint32_t slot = mu_input_key_slot(s, key);
    return slot != MU_INPUT_SLOT_NONE && mu_input_control_pressed(s, slot);
}

MU_INLINE bool mu_input_key_released(const MuInputState *s, MuKey key)
{
    uint32_t slot = mu_input_key_slot(s, key);
    return slot != MU_INPUT_SLOT_NONE && mu_input_control_released(s, slot);
}

/* ---- simple API: mouse buttons by enum ---- */

MU_INLINE bool mu_input_mouse_down(const MuInputState *s, MuMouseButton button)
{
    if ((uint32_t)button >= (uint32_t)MU_MOUSE_COUNT) return false;
    return mu_input_mouse_button_down(s, (uint32_t)button);
}

MU_INLINE bool mu_input_mouse_pressed(const MuInputState *s, MuMouseButton button)
{
    if ((uint32_t)button >= (uint32_t)MU_MOUSE_COUNT) return false;
    return mu_input_mouse_button_pressed(s, (uint32_t)button);
}

MU_INLINE bool mu_input_mouse_released(const MuInputState *s, MuMouseButton button)
{
    if ((uint32_t)button >= (uint32_t)MU_MOUSE_COUNT) return false;
    return mu_input_mouse_button_released(s, (uint32_t)button);
}

MU_INLINE bool mu_input_pad_button_down(const MuInputState *s, uint32_t pad, uint32_t button)
{
    if (pad >= MU_INPUT_MAX_PADS || button >= MU_INPUT_PAD_BUTTONS) return false;
    return (bool)((s->pad_buttons[pad] >> button) & 1u);
}

MU_INLINE bool mu_input_pad_button_pressed(const MuInputState *s, uint32_t pad, uint32_t button)
{
    if (pad >= MU_INPUT_MAX_PADS || button >= MU_INPUT_PAD_BUTTONS) return false;
    return (bool)((((s->pad_buttons[pad] & ~s->pad_buttons_prev[pad]) |
                    s->pad_pressed_frame[pad]) >> button) & 1u);
}

MU_INLINE float mu_input_pad_axis_value(const MuInputState *s, uint32_t pad, uint32_t axis)
{
    if (pad >= MU_INPUT_MAX_PADS || axis >= MU_INPUT_PAD_AXES) return 0.0f;
    return s->pad_axes[pad][axis];
}

/* Deadzone with rescale: below the deadzone reads 0, and full deflection still
 * reads 1.0, so gameplay never has to compensate for the gap. */
MU_INLINE float mu_input_deadzone_f32(float v, float dead)
{
    float a;

    if (dead <= 0.0f) return v;
    if (dead >= 1.0f) return 0.0f;
    a = fabsf(v);
    if (a <= dead) return 0.0f;
    a = (a - dead) / (1.0f - dead);
    return (v < 0.0f) ? -a : a;
}

/* Sloped curve: min + (1-min)*t^2 after the deadzone rescale. Keeps slow
 * movement controllable (roadmap §2 deadzone shapes). */
MU_INLINE float mu_input_deadzone_slope_f32(float v, float dead, float min)
{
    float t = mu_input_deadzone_f32(v, dead);
    float a = fabsf(t);
    float shaped = min + (1.0f - min) * a * a;
    return (t < 0.0f) ? -shaped : shaped;
}

/* Radial deadzone for a stick: magnitude decides, angle is preserved. */
MU_INLINE void mu_input_deadzone2_f32(float *x, float *y, float dead)
{
    float len2, len, scale;

    len2 = (*x) * (*x) + (*y) * (*y);
    if (len2 <= dead * dead) { *x = 0.0f; *y = 0.0f; return; }
    len = sqrtf(len2);
    scale = ((len - dead) / (1.0f - dead)) / len;
    *x *= scale;
    *y *= scale;
}

MU_INLINE float mu_input_sign_f32(float v)
{
    return (v > 0.0f) ? 1.0f : ((v < 0.0f) ? -1.0f : 0.0f);
}

/* ==========================================================================
 * §6 MAP — actions as data (rebindable at runtime; ids stay constants)
 * ========================================================================== */

typedef enum MuInputSourceKind
{
    MU_INPUT_SRC_CONTROL = 0,   /* keyboard or pointer slot                     */
    MU_INPUT_SRC_PAD_BUTTON,
    MU_INPUT_SRC_PAD_AXIS,      /* signed axis: stick direction or trigger      */
    MU_INPUT_SRC_KIND_COUNT
} MuInputSourceKind;

/* Slot identity hint, for rebinding UIs and for backends that can re-resolve on
 * a keyboard-layout change. The core never interprets it. */
#define MU_INPUT_SRC_SYMBOL 0x01u  /* slot came from a layout symbol (e.g. 'W') */
#define MU_INPUT_SRC_POSITION 0x00u /* slot is a physical position (default)    */

/* Deadzone curve per source (stored in flags bits 1..2). */
#define MU_INPUT_CURVE_LINEAR 0u
#define MU_INPUT_CURVE_SLOPE  1u
#define MU_INPUT_CURVE_RADIAL 2u
#define MU_INPUT_CURVE_MASK   0x06u
/* deadzone byte: 0xFF = use MuInputState.deadzone, else deadzone = byte/255. */
#define MU_INPUT_DEADZONE_USE_GLOBAL 0xFFu

typedef struct MuInputSource
{
    uint16_t kind;   /* MuInputSourceKind                                        */
    uint16_t code;   /* control slot / pad button / pad axis                     */
    int8_t   dir;    /* +1 or -1: invert an axis, or read a trigger backwards    */
    int8_t   pad;    /* pad index for pad sources; -1 for controls               */
    uint8_t  flags;  /* MU_INPUT_SRC_SYMBOL/_POSITION + curve in bits 1..2       */
    uint8_t  deadzone; /* 0xFF = global, else deadzone = deadzone/255.0f        */
} MuInputSource;

typedef struct MuInputMap
{
    MuInputSource sources[MU_INPUT_MAX_ACTIONS][MU_INPUT_MAX_SOURCES];
    uint8_t       source_count[MU_INPUT_MAX_ACTIONS];
    uint16_t      action_count;
} MuInputMap;

MU_INLINE void mu_input_map_init(MuInputMap *map, uint16_t action_count)
{
    memset(map, 0, sizeof *map);
    map->action_count = (action_count > MU_INPUT_MAX_ACTIONS)
                      ? (uint16_t)MU_INPUT_MAX_ACTIONS : action_count;
}

MU_INLINE bool mu_input_bind(MuInputMap *map, uint32_t action, MuInputSourceKind kind,
                             uint32_t code, int32_t dir, int32_t pad, uint8_t flags)
{
    MuInputSource *src;

    if (action >= map->action_count) return false;
    if ((uint32_t)kind >= (uint32_t)MU_INPUT_SRC_KIND_COUNT) return false;
    if (map->source_count[action] >= MU_INPUT_MAX_SOURCES) return false;
    src = &map->sources[action][map->source_count[action]++];
    src->kind  = (uint16_t)kind;
    src->code  = (uint16_t)code;
    src->dir   = (int8_t)(dir < 0 ? -1 : 1);
    src->pad   = (int8_t)(pad < 0 ? -1 : pad);
    src->flags = flags;
    src->deadzone = MU_INPUT_DEADZONE_USE_GLOBAL;
    return true;
}

MU_INLINE bool mu_input_bind_control(MuInputMap *map, uint32_t action, uint32_t slot)
{
    return mu_input_bind(map, action, MU_INPUT_SRC_CONTROL, slot, 1, -1, MU_INPUT_SRC_POSITION);
}

/* Bind by logical key: resolves MuKey -> slot via the state's key_slot map.
 * Returns false if the key is unmapped (backend forgot to map it). */
MU_INLINE bool mu_input_bind_key(MuInputMap *map, uint32_t action,
                                 const MuInputState *s, MuKey key)
{
    uint32_t slot = mu_input_key_slot(s, key);
    if (slot == MU_INPUT_SLOT_NONE) return false;
    return mu_input_bind_control(map, action, slot);
}

MU_INLINE bool mu_input_bind_pad_button(MuInputMap *map, uint32_t action, uint32_t pad,
                                        uint32_t button)
{
    return mu_input_bind(map, action, MU_INPUT_SRC_PAD_BUTTON, button, 1, (int32_t)pad, 0u);
}

MU_INLINE bool mu_input_bind_pad_axis(MuInputMap *map, uint32_t action, uint32_t pad,
                                      uint32_t axis, int32_t dir)
{
    return mu_input_bind(map, action, MU_INPUT_SRC_PAD_AXIS, axis, dir, (int32_t)pad, 0u);
}

/* Pad axis with explicit deadzone (0..1) and curve (MU_INPUT_CURVE_*). */
MU_INLINE bool mu_input_bind_pad_axis_ex(MuInputMap *map, uint32_t action, uint32_t pad,
                                         uint32_t axis, int32_t dir,
                                         float deadzone, uint32_t curve)
{
    MuInputSource *src;
    if (!mu_input_bind(map, action, MU_INPUT_SRC_PAD_AXIS, axis, dir, (int32_t)pad, 0u))
        return false;
    src = &map->sources[action][map->source_count[action] - 1u];
    if (deadzone < 0.0f) deadzone = 0.0f;
    if (deadzone > 1.0f) deadzone = 1.0f;
    src->deadzone = (uint8_t)(deadzone * 255.0f + 0.5f);
    src->flags = (uint8_t)((src->flags & (uint8_t)~MU_INPUT_CURVE_MASK) |
                           ((curve & 3u) << 1));
    return true;
}

MU_INLINE void mu_input_action_unbind(MuInputMap *map, uint32_t action)
{
    if (action >= map->action_count) return;
    map->source_count[action] = 0u;
}

/* ---- chords: two actions pressed within a tick window ---- */

typedef struct MuInputChord
{
    uint32_t a;
    uint32_t b;
    uint32_t window_ticks;
} MuInputChord;

typedef struct MuInputChords
{
    MuInputChord items[MU_INPUT_MAX_CHORDS];
    uint32_t     count;
} MuInputChords;

MU_INLINE void mu_input_chords_init(MuInputChords *c)
{
    memset(c, 0, sizeof *c);
}

MU_INLINE bool mu_input_chord_add(MuInputChords *c, uint32_t a, uint32_t b,
                                  uint32_t window_ticks)
{
    if (c->count >= MU_INPUT_MAX_CHORDS) return false;
    c->items[c->count].a = a;
    c->items[c->count].b = b;
    c->items[c->count].window_ticks = window_ticks;
    c->count++;
    return true;
}

/* ==========================================================================
 * §7 RESOLVE — one pass per frame, then gameplay reads plain arrays
 * ========================================================================== */

typedef struct MuInputActions
{
    uint64_t down[MU_INPUT_ACTION_WORDS];
    uint64_t down_prev[MU_INPUT_ACTION_WORDS];
    uint64_t pressed[MU_INPUT_ACTION_WORDS];
    uint64_t released[MU_INPUT_ACTION_WORDS];
    float    value[MU_INPUT_MAX_ACTIONS];      /* analog; 1.0 for digital      */
    float    held_time[MU_INPUT_MAX_ACTIONS];  /* seconds held; 0 when up      */
    uint32_t last_pressed_tick[MU_INPUT_MAX_ACTIONS]; /* tick of last press   */
    float    mouse_dx, mouse_dy;       /* copied from state (actions-only)   */
    float    wheel_x, wheel_y;         /* copied from state (actions-only)   */
    uint32_t tick;                     /* state tick this snapshot is for    */
    uint16_t action_count;
} MuInputActions;

/* Tick-quantised snapshot for fixed-step sims (deterministic, replayable).
 * Sample once per SIM tick with mu_input_sample_tick(); the caller keeps
 * the frame alive across ticks so held_ticks / last_pressed_tick accumulate. */
typedef struct MuInputFrame
{
    uint64_t down[MU_INPUT_ACTION_WORDS];
    uint64_t pressed[MU_INPUT_ACTION_WORDS];
    uint64_t released[MU_INPUT_ACTION_WORDS];
    uint64_t chord_pressed[MU_INPUT_ACTION_WORDS]; /* bit i = chord i fired  */
    float    value[MU_INPUT_MAX_ACTIONS];
    uint32_t held_ticks[MU_INPUT_MAX_ACTIONS];
    uint32_t last_pressed_tick[MU_INPUT_MAX_ACTIONS];
    float    mouse_dx, mouse_dy;
    float    wheel_x, wheel_y;
    uint32_t tick;
    uint16_t action_count;
    uint16_t chord_count;
} MuInputFrame;

MU_INLINE float mu_input_source_value(const MuInputState *s, const MuInputSource *src)
{
    switch (src->kind)
    {
    case MU_INPUT_SRC_CONTROL:
        return mu_input_control_down(s, src->code) ? 1.0f : 0.0f;
    case MU_INPUT_SRC_PAD_BUTTON:
        return mu_input_pad_button_down(s, (uint32_t)(src->pad < 0 ? 0 : src->pad), src->code)
             ? 1.0f : 0.0f;
    case MU_INPUT_SRC_PAD_AXIS:
    {
        uint32_t pad = (uint32_t)(src->pad < 0 ? 0 : src->pad);
        float v = s->pad_axes[pad][src->code] * (float)src->dir;
        float dead = (src->deadzone == MU_INPUT_DEADZONE_USE_GLOBAL)
                   ? s->deadzone : ((float)src->deadzone / 255.0f);
        uint32_t curve = (uint32_t)((src->flags & MU_INPUT_CURVE_MASK) >> 1);
        if (curve == MU_INPUT_CURVE_SLOPE)
            v = mu_input_deadzone_slope_f32(v, dead, 0.15f);
        else
            v = mu_input_deadzone_f32(v, dead);
        if (v < 0.0f) v = 0.0f;            /* a direction, not a magnitude */
        return v;
    }
    default:
        return 0.0f;
    }
}

/* Held-or-tapped this frame. A source that went down and back up inside one
 * frame is not held, but it was pressed, and gameplay must see that. */
MU_INLINE bool mu_input_source_pressed(const MuInputState *s, const MuInputSource *src)
{
    switch (src->kind)
    {
    case MU_INPUT_SRC_CONTROL:
        return mu_input_control_pressed(s, src->code);
    case MU_INPUT_SRC_PAD_BUTTON:
        return mu_input_pad_button_pressed(s, (uint32_t)(src->pad < 0 ? 0 : src->pad), src->code);
    case MU_INPUT_SRC_PAD_AXIS:
        return false;   /* an axis has no edge; its magnitude is the signal */
    default:
        return false;
    }
}

MU_INLINE void mu_input_actions_reset(MuInputActions *a)
{
    memset(a, 0, sizeof *a);
}

MU_INLINE void mu_input_frame_reset(MuInputFrame *f)
{
    memset(f, 0, sizeof *f);
}

MU_INLINE void mu_input_resolve(const MuInputState *MU_RESTRICT s,
                                const MuInputMap *MU_RESTRICT map,
                                MuInputActions *MU_RESTRICT out, float dt)
{
    uint64_t down[MU_INPUT_ACTION_WORDS];
    uint64_t tap[MU_INPUT_ACTION_WORDS];   /* pressed and released inside this frame */
    uint32_t action, i, w;

    out->action_count = map->action_count;
    out->tick = s->tick;
    out->mouse_dx = s->mouse_dx;
    out->mouse_dy = s->mouse_dy;
    out->wheel_x = s->wheel_x;
    out->wheel_y = s->wheel_y;
    for (w = 0; w < MU_INPUT_ACTION_WORDS; ++w) { down[w] = 0u; tap[w] = 0u; }

    for (action = 0; action < map->action_count; ++action)
    {
        const MuInputSource *src = map->sources[action];
        const uint32_t count = map->source_count[action];
        float value = 0.0f;
        bool is_down = false;
        bool tapped = false;

        for (i = 0; i < count; ++i)
        {
            const float v = mu_input_source_value(s, &src[i]);
            if (v > value) value = v;
            if (v > 0.0f) is_down = true;
            if (mu_input_source_pressed(s, &src[i])) tapped = true;
        }

        if (is_down) down[action >> 6] |= 1ull << (action & 63u);
        /* Only a tap that already ended needs the latch; one still down is an
         * ordinary edge against down_prev below. */
        if (tapped && !is_down) tap[action >> 6] |= 1ull << (action & 63u);

        out->value[action] = value;
        out->held_time[action] = is_down ? out->held_time[action] + dt : 0.0f;
        if (tapped) out->last_pressed_tick[action] = s->tick;
    }

    for (w = 0; w < MU_INPUT_ACTION_WORDS; ++w)
    {
        out->down_prev[w] = out->down[w];
        out->down[w]      = down[w];
        out->pressed[w]   = (down[w] & ~out->down_prev[w]) | tap[w];
        out->released[w]  = (~down[w] & out->down_prev[w]) | tap[w];
    }
}

/* Sample one SIM tick into a persistent MuInputFrame. Unlike resolve(),
 * held time is in ticks and last_pressed_tick is in ticks, so the result
 * replays bit-identically. Chords (if chords != NULL) are resolved here:
 * chord i fires on the tick its second member is pressed, iff the first
 * member was pressed within window_ticks and both are still down. */
MU_INLINE void mu_input_sample_tick(const MuInputState *MU_RESTRICT s,
                                    const MuInputMap *MU_RESTRICT map,
                                    const MuInputChords *MU_RESTRICT chords,
                                    MuInputFrame *MU_RESTRICT out, uint32_t tick)
{
    uint64_t down[MU_INPUT_ACTION_WORDS];
    uint64_t tap[MU_INPUT_ACTION_WORDS];
    uint32_t action, i, w;

    out->action_count = map->action_count;
    out->tick = tick;
    out->mouse_dx = s->mouse_dx;
    out->mouse_dy = s->mouse_dy;
    out->wheel_x = s->wheel_x;
    out->wheel_y = s->wheel_y;
    for (w = 0; w < MU_INPUT_ACTION_WORDS; ++w) { down[w] = 0u; tap[w] = 0u; out->chord_pressed[w] = 0u; }

    for (action = 0; action < map->action_count; ++action)
    {
        const MuInputSource *src = map->sources[action];
        const uint32_t count = map->source_count[action];
        float value = 0.0f;
        bool is_down = false;
        bool tapped = false;

        for (i = 0; i < count; ++i)
        {
            const float v = mu_input_source_value(s, &src[i]);
            if (v > value) value = v;
            if (v > 0.0f) is_down = true;
            if (mu_input_source_pressed(s, &src[i])) tapped = true;
        }

        if (is_down) down[action >> 6] |= 1ull << (action & 63u);
        if (tapped && !is_down) tap[action >> 6] |= 1ull << (action & 63u);

        out->value[action] = value;
        out->held_ticks[action] = is_down ? out->held_ticks[action] + 1u : 0u;
        if (tapped) out->last_pressed_tick[action] = tick;
    }

    for (w = 0; w < MU_INPUT_ACTION_WORDS; ++w)
    {
        out->pressed[w]   = (down[w] & ~out->down[w]) | tap[w];
        out->released[w]  = (~down[w] & out->down[w]) | tap[w];
        out->down[w]      = down[w];
    }

    out->chord_count = chords ? chords->count : 0u;
    if (chords)
    {
        for (i = 0; i < chords->count; ++i)
        {
            uint32_t a = chords->items[i].a;
            uint32_t b = chords->items[i].b;
            uint32_t win = chords->items[i].window_ticks;
            bool a_down, b_down, a_pressed, b_pressed;
            bool fire = false;

            if (a >= MU_INPUT_MAX_ACTIONS || b >= MU_INPUT_MAX_ACTIONS) continue;
            a_down = (bool)((out->down[a >> 6] >> (a & 63u)) & 1ull);
            b_down = (bool)((out->down[b >> 6] >> (b & 63u)) & 1ull);
            a_pressed = (bool)((out->pressed[a >> 6] >> (a & 63u)) & 1ull);
            b_pressed = (bool)((out->pressed[b >> 6] >> (b & 63u)) & 1ull);
            if (a_down && b_down)
            {
                if (b_pressed && (tick - out->last_pressed_tick[a]) <= win) fire = true;
                if (a_pressed && (tick - out->last_pressed_tick[b]) <= win) fire = true;
            }
            if (fire) out->chord_pressed[i >> 6] |= 1ull << (i & 63u);
        }
    }
}

/* ==========================================================================
 * §8 ACTIONS — what gameplay asks (all inline, all one or two loads)
 * ========================================================================== */

MU_INLINE bool mu_input_action_down(const MuInputActions *a, uint32_t action)
{
    if (action >= MU_INPUT_MAX_ACTIONS) return false;
    return (bool)((a->down[action >> 6] >> (action & 63u)) & 1ull);
}

MU_INLINE bool mu_input_action_pressed(const MuInputActions *a, uint32_t action)
{
    if (action >= MU_INPUT_MAX_ACTIONS) return false;
    return (bool)((a->pressed[action >> 6] >> (action & 63u)) & 1ull);
}

MU_INLINE bool mu_input_action_released(const MuInputActions *a, uint32_t action)
{
    if (action >= MU_INPUT_MAX_ACTIONS) return false;
    return (bool)((a->released[action >> 6] >> (action & 63u)) & 1ull);
}

MU_INLINE float mu_input_action_value(const MuInputActions *a, uint32_t action)
{
    if (action >= MU_INPUT_MAX_ACTIONS) return 0.0f;
    return a->value[action];
}

/* Buffered: was this action pressed within the last window_ticks ticks?
 * O(1) — no history scan. Uses last_pressed_tick written by resolve(). */
MU_INLINE bool mu_input_action_buffered(const MuInputActions *a, uint32_t action,
                                        uint32_t window_ticks)
{
    if (action >= MU_INPUT_MAX_ACTIONS) return false;
    return (a->tick - a->last_pressed_tick[action]) <= window_ticks;
}

/* -1 / 0 / +1 from an opposed pair of actions; analog-aware, so a stick and a
 * key pair produce the same signal. */
MU_INLINE float mu_input_axis(const MuInputActions *a, uint32_t negative, uint32_t positive)
{
    return mu_input_action_value(a, positive) - mu_input_action_value(a, negative);
}

/* Menu / text-entry repeat: fires once on press, then after `delay` seconds and
 * every 1/rate seconds while held. Stateless — timing lives in held_time. */
MU_INLINE bool mu_input_action_repeat(const MuInputActions *a, uint32_t action,
                                      float delay, float rate, float dt)
{
    float held, prev;

    if (action >= MU_INPUT_MAX_ACTIONS || rate <= 0.0f) return false;
    held = a->held_time[action];
    if (held < delay) return false;
    prev = held - dt;
    if (prev < delay) return true;                    /* crossed the delay */
    return floorf((prev - delay) * rate) != floorf((held - delay) * rate);
}

/* ==========================================================================
 * §9 FRAME — tick-snapshot queries (buffered / coyote / chords)
 * ========================================================================== */

MU_INLINE bool mu_input_frame_down(const MuInputFrame *f, uint32_t action)
{
    if (action >= MU_INPUT_MAX_ACTIONS) return false;
    return (bool)((f->down[action >> 6] >> (action & 63u)) & 1ull);
}

MU_INLINE bool mu_input_frame_pressed(const MuInputFrame *f, uint32_t action)
{
    if (action >= MU_INPUT_MAX_ACTIONS) return false;
    return (bool)((f->pressed[action >> 6] >> (action & 63u)) & 1ull);
}

MU_INLINE bool mu_input_frame_released(const MuInputFrame *f, uint32_t action)
{
    if (action >= MU_INPUT_MAX_ACTIONS) return false;
    return (bool)((f->released[action >> 6] >> (action & 63u)) & 1ull);
}

MU_INLINE float mu_input_frame_value(const MuInputFrame *f, uint32_t action)
{
    if (action >= MU_INPUT_MAX_ACTIONS) return 0.0f;
    return f->value[action];
}

/* Pressed within the last window_ticks ticks (inclusive). O(1). */
MU_INLINE bool mu_input_buffered(const MuInputFrame *f, uint32_t action, uint32_t window_ticks)
{
    if (action >= MU_INPUT_MAX_ACTIONS) return false;
    return (f->tick - f->last_pressed_tick[action]) <= window_ticks;
}

/* Coyote time: same query, named for the jump case (pressed shortly before
 * leaving the ground still counts). */
MU_INLINE bool mu_input_coyote(const MuInputFrame *f, uint32_t action, uint32_t window_ticks)
{
    return mu_input_buffered(f, action, window_ticks);
}

MU_INLINE bool mu_input_chord_pressed(const MuInputFrame *f, uint32_t chord)
{
    if (chord >= MU_INPUT_MAX_CHORDS) return false;
    return (bool)((f->chord_pressed[chord >> 6] >> (chord & 63u)) & 1ull);
}

/* Tick repeat: fires once on press, then after delay_ticks and every
 * period_ticks while held. Stateless — timing lives in held_ticks. */
MU_INLINE bool mu_input_frame_repeat(const MuInputFrame *f, uint32_t action,
                                     uint32_t delay_ticks, uint32_t period_ticks)
{
    uint32_t held;
    if (action >= MU_INPUT_MAX_ACTIONS || period_ticks == 0u) return false;
    held = f->held_ticks[action];
    if (held < delay_ticks) return false;
    if (held == delay_ticks) return true;             /* crossed the delay */
    return ((held - delay_ticks) % period_ticks) == 0u;
}

#endif /* MU_INPUT_H */
