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
 * NOT PROVIDED ON PURPOSE (v1)
 * ----------------------------
 * Text/IME (needs platform composition), touch, gestures, haptics, and
 * binding persistence (mu_input_map_write/parse). All are additive; none of
 * them changes a line of gameplay code because gameplay only reads actions.
 */

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
 * §2 EVENTS — what the backend observed
 * ========================================================================== */

typedef enum MuInputEventKind
{
    MU_INPUT_EV_CONTROL_DOWN = 0,   /* keyboard or pointer button pressed   */
    MU_INPUT_EV_CONTROL_UP,         /* ... and released                     */
    MU_INPUT_EV_PAD_BUTTON_DOWN,
    MU_INPUT_EV_PAD_BUTTON_UP,
    MU_INPUT_EV_WHEEL,              /* value = wheel clicks, + is up/right  */
    MU_INPUT_EV_KIND_COUNT
} MuInputEventKind;

typedef struct MuInputEvent
{
    uint64_t time_ns;   /* when it happened, not when the frame read it     */
    uint32_t code;      /* control slot / pad button / (unused for wheel)   */
    uint16_t kind;      /* MuInputEventKind                                 */
    int16_t  pad;       /* pad index, -1 for keyboard/pointer               */
    float    value;     /* wheel delta; 0.0 for discrete events             */
} MuInputEvent;

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

    float    mouse_x, mouse_y;
    float    mouse_prev_x, mouse_prev_y;
    float    mouse_dx, mouse_dy;       /* derived each frame; what games want  */
    float    wheel_x, wheel_y;         /* this frame's wheel delta             */
    float    wheel_pending_x, wheel_pending_y;

    float    deadzone;                 /* analog: below this, an axis is zero  */
    float    dt;                       /* clamped frame delta, seconds         */
    uint64_t time_ns;

    MuInputEvent events[MU_INPUT_EVENT_CAP];
    uint32_t     event_count;
    uint32_t     event_dropped;        /* ring overflow; a bug worth surfacing */
} MuInputState;

/* ==========================================================================
 * §4 STATE — backend API
 * ========================================================================== */

MU_INLINE bool mu_input_push(MuInputState *s, MuInputEventKind kind, uint32_t code,
                             int32_t pad, float value, uint64_t time_ns)
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
    e->kind    = (uint16_t)kind;
    e->pad     = (int16_t)pad;
    e->value   = value;
    return true;
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

MU_INLINE void mu_input_wheel_add(MuInputState *s, float dx, float dy)
{
    mu_input_push(s, MU_INPUT_EV_WHEEL, 0u, -1, dy, s->time_ns);
    s->wheel_pending_x += dx;
    s->wheel_pending_y += dy;
}

/* Continuous state is pushed directly: it has no edges worth queueing. */
MU_INLINE void mu_input_mouse_pos(MuInputState *s, float x, float y)
{
    s->mouse_x = x;
    s->mouse_y = y;
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

MU_INLINE void mu_input_init(MuInputState *s)
{
    memset(s, 0, sizeof *s);
    s->deadzone = 0.15f;
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
            break; /* already accumulated in wheel_pending by the backend */
        default:
            break;
        }
    }
    s->event_count = 0;

    dt = (float)((double)(now_ns - s->time_ns) * 1e-9);
    if (now_ns < s->time_ns) dt = 0.0f;
    if (dt > MU_INPUT_MAX_DT) dt = MU_INPUT_MAX_DT;
    s->dt = dt;
    s->time_ns = now_ns;

    s->mouse_dx = s->mouse_x - s->mouse_prev_x;
    s->mouse_dy = s->mouse_y - s->mouse_prev_y;
    s->mouse_prev_x = s->mouse_x;
    s->mouse_prev_y = s->mouse_y;

    s->wheel_x = s->wheel_pending_x;
    s->wheel_y = s->wheel_pending_y;
    s->wheel_pending_x = 0.0f;
    s->wheel_pending_y = 0.0f;
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

typedef struct MuInputSource
{
    uint16_t kind;   /* MuInputSourceKind                                        */
    uint16_t code;   /* control slot / pad button / pad axis                     */
    int8_t   dir;    /* +1 or -1: invert an axis, or read a trigger backwards    */
    int8_t   pad;    /* pad index for pad sources; -1 for controls               */
    uint8_t  flags;  /* MU_INPUT_SRC_SYMBOL / _POSITION (advisory)               */
    uint8_t  reserved;
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
    src->reserved = 0u;
    return true;
}

MU_INLINE bool mu_input_bind_control(MuInputMap *map, uint32_t action, uint32_t slot)
{
    return mu_input_bind(map, action, MU_INPUT_SRC_CONTROL, slot, 1, -1, MU_INPUT_SRC_POSITION);
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

MU_INLINE void mu_input_action_unbind(MuInputMap *map, uint32_t action)
{
    if (action >= map->action_count) return;
    map->source_count[action] = 0u;
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
    uint16_t action_count;
} MuInputActions;

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
        v = mu_input_deadzone_f32(v, s->deadzone);
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

MU_INLINE void mu_input_resolve(const MuInputState *s, const MuInputMap *map,
                                MuInputActions *out, float dt)
{
    uint64_t down[MU_INPUT_ACTION_WORDS];
    uint64_t tap[MU_INPUT_ACTION_WORDS];   /* pressed and released inside this frame */
    uint32_t action, i, w;

    out->action_count = map->action_count;
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
    }

    for (w = 0; w < MU_INPUT_ACTION_WORDS; ++w)
    {
        out->down_prev[w] = out->down[w];
        out->down[w]      = down[w];
        out->pressed[w]   = (down[w] & ~out->down_prev[w]) | tap[w];
        out->released[w]  = (~down[w] & out->down_prev[w]) | tap[w];
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

#endif /* MU_INPUT_H */
