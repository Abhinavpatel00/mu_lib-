# `mu_input.h` — device state, timestamped events, actions as data

A single-header C99 input layer: a **device state** that records what happened
(ordered, timestamped), an **action map** that says what it means (bindings are
plain data), and a **per-frame resolve** that hands the game one immutable
snapshot.

```c
#include "mu/mu_input.h"        /* needs mu_common.h, mu_math.h; link -lm */

mu_input_init(&state);                       /* caller owns the memory      */
mu_input_frame_begin(&state, frame_ns);      /* once per frame, first       */
   /* ... push_control / push_pad_* from the platform adapter ... */
mu_input_resolve(&state, &map, &actions, state.dt);

if (mu_input_action_pressed(&actions, ACT_QUIT))  break;
float move = mu_input_axis(&actions, ACT_LEFT, ACT_RIGHT);
```

### Simple / quick-check API (no map needed)

For small demos or "just check if W is down" code, skip the map entirely.
`MuKey` / `MuMouseButton` are logical, platform-free enums; the backend maps
each `MuKey` to a physical slot once at startup (the GLFW adapter does this
for you, so this works with zero setup in a real app):

```c
mu_input_init(&state);
mu_input_frame_begin(&state, frame_ns);
   /* ... push_control / push_pad_* from the platform adapter ... */

if (mu_input_key_pressed(&state, MU_KEY_ESCAPE)) break;   // edge, one frame
if (mu_input_key_down(&state, MU_KEY_W))          move();  // held
if (mu_input_key_pressed(&state, MU_KEY_C))       crouch();// tap-safe edge
if (mu_input_mouse_down(&state, MU_MOUSE_RIGHT))  drag();  // held
if (mu_input_mouse_pressed(&state, MU_MOUSE_LEFT)) click();// edge
```

In tests, `mu_input_key_map_identity(&state)` maps `MU_KEY_A -> slot MU_KEY_A`,
so the enum value *is* the slot. In a backend, call
`mu_input_key_set_slot(&state, MU_KEY_W, scancode_of_W)` once per key.
Unmapped keys read `false` (never crash). `mu_input_bind_key(&map, ACT_JUMP,
&state, MU_KEY_SPACE)` binds an action by logical key, resolving via the
same map — so the simple and full APIs share one mapping.

No allocation, no globals, no OS headers, no `IMPLEMENTATION` macro: the whole
header is `MU_INLINE`, so `MU_STATIC` / `MU_DLL_EXPORT` behave as everywhere
else in `mu/`. It has never seen a window — GLFW lives in one adapter file at
the edge of the program and talks to this header through `mu_input_push_*`.

## Why it exists

Three bugs, all of them ordinary, all of them expensive in a game:

1. **The lost tap.** State polling ("is the key down *now*?") cannot see a key
   that went down and back up between two polls. With an 8 ms frame that is
   rare; on a 120 ms hitch, or a *tap* shorter than one frame, it just
   disappears. This header keeps an ordered event ring and a per-frame latch, so
   a tap that started and ended inside the frame is still delivered — exactly
   once.
2. **The invisible binding change.** Code that tests `GLFW_KEY_W` is code that
   breaks on AZERTY. Bindings are *data* here: `mu_input_bind_control(&map,
   ACT_ZOOM_IN, slot)` where `slot` is a **scancode**, not a key code. The demo
   stores positions and prints names (`platform_slot_label` resolves `W` to `Z`
   on a French layout).
3. **The frame that ran twice.** `dt` from the platform is clamped to
   `MU_INPUT_MAX_DT` (0.1 s) at the source, so a debugger pause or a stall
   cannot teleport the player through a wall.

Both of the first two were caught live in this repo, not in review — see
[Evidence](#evidence-what-the-live-run-caught).

## The three layers

| Layer | Type | Lifetime | Contents |
|---|---|---|---|
| Device state | `MuInputState` | one per process | control bitsets (8×u64), mouse buttons + delta + wheel, 4 pads + connect bits, `MuKey -> slot` map, the event ring, `dt`, `time_ns`, `tick` |
| Binding | `MuInputMap` | one per player | up to 128 actions × up to 4 sources, each a `MuInputSource` (8 B) |
| Snapshot | `MuInputActions` | one per frame | down / down_prev / pressed / released bitsets, `value[128]`, `held_time[128]`, `last_pressed_tick[128]`, mouse/wheel copy |
| Tick snapshot | `MuInputFrame` | one per sim tick | as above but `held_ticks` / `last_pressed_tick` in ticks + `chord_pressed` bitset |

```c
typedef struct MuInputSource {
    uint16_t kind;     /* CONTROL / PAD_BUTTON / PAD_AXIS */
    uint16_t code;     /* control slot / pad button / pad axis */
    int8_t   dir;      /* +1 / -1: invert an axis */
    int8_t   pad;      /* pad index, -1 for controls */
    uint8_t  flags;    /* SYMBOL/POSITION + curve (bits 1..2) */
    uint8_t  deadzone; /* 0xFF = global, else deadzone/255 */
} MuInputSource; /* 8 B */
```

`kind` is `CONTROL` (a keyboard/mouse slot), `PAD_BUTTON` or `PAD_AXIS`;
`dir = -1` inverts an axis so "stick up" and "W" can drive the same action.
`flags` is advisory metadata (`SYMBOL` / `POSITION`) used only for UI prompts,
plus the deadzone curve (`LINEAR` / `SLOPE` / `RADIAL`). `deadzone` is
per-source (0xFF = use `MuInputState.deadzone`).

### Frame order

```
prev ← held;  pressed_frame ← 0        (memory of the previous frame)
apply queued events in order           (DOWN sets held|pressed_frame, UP clears held)
dt = min(now - last, MU_INPUT_MAX_DT)  (stall clamp)
mouse delta from the latch
resolve: actions from state
```

`pressed` is computed as `(held & ~prev) | pressed_frame`, so an in-frame tap
survives into the snapshot for exactly one frame, and `down` is honestly false
because the key really is up.

### Why a timestamp per event

Every queued event carries `time_ns`. Ordering *within* a frame and ordering
*across* devices is preserved, which is what a replay file needs: the ring is
already the record format. Buffer windows, chords and coyote time (roadmap item
2) all read the same stream; none of them need a different data structure.

## Actions as data

```c
mu_input_map_init(&map, ACT_COUNT);
mu_input_bind_control(&map, ACT_ORBIT_LEFT,  slot_of("A"));           /* position  */
mu_input_bind_control(&map, ACT_ORBIT_RIGHT, slot_of("D"));
mu_input_bind_pad_axis(&map, ACT_ORBIT_RIGHT, 0, MU_INPUT_PAD_LX, +1);
mu_input_bind_pad_axis(&map, ACT_ORBIT_LEFT,  0, MU_INPUT_PAD_LX, -1);
mu_input_bind_pad_button(&map, ACT_SPRINT, 0, MU_INPUT_PAD_L3);
```

Several sources may drive one action; they are **OR**-ed for edges and
**max**-ed for value, so a keyboard and a stick on the same axis never fight.
An action with no sources reads `0` forever rather than falling back to
anything. Per-player maps are `MuInputMap` copies, so local multiplayer is not a
retrofit.

Deadzones are per-axis, not per-action, and normalise rather than clip:
`0.575` with `dead = 0.25` becomes `0.5`, not `0.325` — slow movement stays
controllable instead of collapsing into a step. `mu_input_deadzone2_f32` is the
radial (stick) form.

## Repeat, without state

`mu_input_action_repeat` is a pure function of `held_time`, so it is safe to
call from anywhere and cannot desynchronise:

```c
if (held < delay)                       return false;   /* not yet           */
if (held - dt < delay)                  return true;    /* crossed the delay */
return floorf((prev - delay) * rate) != floorf((held - delay) * rate);
```

No fire on the press frame, one fire on the boundary, then one per `1/rate`.

## API

| Function | Purpose |
|---|---|
| `mu_input_init` / `mu_input_actions_reset` / `mu_input_frame_reset` | zero the state / snapshots |
| `mu_input_frame_begin(state, ns)` | advance the frame: prev←held, clear latch, clamp `dt`, `tick++` |
| `mu_input_push_control(state, slot, down, ns)` | keyboard/mouse button edge, ordered |
| `mu_input_push_pad_button` / `_pad_axis` / `mu_input_pad_connect(state, pad, connected, ns)` | gamepad edges + lifecycle |
| `mu_input_pad_connected(state, pad)` | is this pad currently connected? |
| `mu_input_wheel_add` / `mu_input_wheel_consume(state, &x, &y)` | event-only wheel + take-once scroll |
| `mu_input_mouse_pos` / `mu_input_mouse_move(state, dx, dy)` / `mu_input_mouse_warp` | absolute / relative / warp (no delta) |
| `mu_input_focus_lost` | drop held keys, no phantom press on regain |
| `mu_input_control_down` / `_pressed` / `_released` | raw slot queries (latch-aware `pressed`) |
| `mu_input_key_down` / `_pressed` / `_released(state, MU_KEY_W)` | **simple API**: logical-key queries, no map |
| `mu_input_mouse_down` / `_pressed` / `_released(state, MU_MOUSE_RIGHT)` | **simple API**: mouse-button queries by enum |
| `mu_input_key_set_slot` / `_slot` / `_map_identity` / `_map_clear` | backend fills `MuKey -> slot` once; identity for tests |
| `mu_input_events(state)` / `mu_input_replay_events(state, span)` | record/replay: copy the span before `frame_begin` drains it |
| `mu_input_map_init` / `_bind_control` / `_bind_key` / `_bind_pad_button` / `_bind_pad_axis` / `_bind_pad_axis_ex` | bindings as data (by slot, by logical key, or axis with deadzone+curve) |
| `mu_input_resolve` / `mu_input_action_*` / `mu_input_axis` / `_repeat` / `_buffered` | the snapshot API (mouse/wheel copied into actions) |
| `mu_input_sample_tick(state, map, chords, frame, tick)` | fixed-step tick sampler (deterministic) |
| `mu_input_buffered` / `mu_input_coyote(frame, action, window_ticks)` | pressed within N ticks? O(1) |
| `mu_input_chords_init` / `_add` / `mu_input_chord_pressed(frame, i)` | chords with a tick resolution window |
| `mu_input_frame_down` / `_pressed` / `_value` / `_repeat` | tick-snapshot queries |
| `mu_input_deadzone_f32` / `_deadzone_slope_f32` / `_deadzone2_f32` / `_sign_f32` | scalar helpers |

Sizes (x86-64, `-O2`, v2): `MuInputState` **8 768 B** (event is now 32 B:
`tick` + wheel `value2`), `MuInputActions` **1 624 B** (`last_pressed_tick` +
mouse/wheel copy), `MuInputFrame` **1 624 B**, `MuInputMap` 4 226 B,
`MuInputSource` 8 B (unchanged). Everything is `MU_INPUT_EVENT_CAP`
256 events deep; past that, pushes are counted in `event_dropped` instead of
silently stitching together a fake ordering. That number is a bug report, not
noise: a frame that drops events is a frame that stalled.

## Cost

Measured on the same machine as the other `mu` headers (`/tmp/bench_in.c`,
`-O2`, 200 000 iterations, best of 3):

| Operation | Cost |
|---|---|
| frame: `frame_begin` + `resolve`, 16 actions, 3 held | **170 ns** |
| the same frame after pushing 200 events | 1 115 ns (5.6 ns/event) |
| `mu_input_action_pressed` | 0.72 ns |
| `mu_input_axis` (two action lookups) | 2.81 ns |

A pathological 200-event frame costs 0.007 % of a 16.7 ms budget. The library is
free; the ring is the only thing that can grow, and it is fixed at init.

## Adapter: what a backend must provide

The library never names a platform type. A backend implements six things:

| Adapter call | Contract |
|---|---|
| `poll()` once per frame | translate native events to `push_*`, then `frame_begin`, then `resolve` — the *single* definition of the frame boundary |
| `state()` / `actions()` | hand out the two snapshots |
| `map()` | the mutable action map (bind at startup, rebind at runtime) |
| `slot_from_name("W")` / `slot_label(slot, buf, cap)` | name ↔ position, layout-aware |
| `create_surface(instance, &surface)` | WSI for a Vulkan demo |
| `time_ns()` | the clock the timestamps use |

`demos/common/platform.c` is the GLFW implementation and the only file in the
repo that includes `<GLFW/glfw3.h>`. A `platform_input_sdl3.c` with the same six
functions is the planned drop-in (SDL3's event queue is the model this API
copies, GLFW is the weight we kept). The adapter fills the `MuKey -> slot` map
at startup (`fill_key_slots`), so `platform_key_pressed(app, MU_KEY_ESCAPE)`
and `mu_input_key_down(platform_state(app), MU_KEY_W)` work with zero setup.
`platform_poll` is split into `platform_poll_events` / `platform_frame_begin`
/ `platform_resolve` (one clock reading per poll, shared by all callbacks) so
fixed-step sims can sample N ticks per render frame; `platform_init` /
`platform_shutdown` are the caller-owned variants of create/destroy.

Two adapter details worth copying into any backend:

- GLFW reports **no gamepad button events**, so the adapter polls the pad and
  synthesises edges against its own previous frame.
- Key names come from `glfwGetKeyName(GLFW_KEY_UNKNOWN, scancode)`, which
  follows the *user's layout*; scancodes stay stable. Hence "store position,
  print name".

## Gate

`tests/input.c` fabricates input with no window, no GPU and no display, and
asserts:

- edges across frames; a tap inside one frame → `pressed` once, `down` false,
  and **reaches the action layer** (regression for the bug below);
- wheel accumulation and take-once consumption; mouse delta, and no delta after
  a warp; focus loss clearing held with no phantom press;
- a 5 s stall clamped to `MU_INPUT_MAX_DT`;
- ring overflow accounting: exactly `MU_INPUT_EVENT_CAP` events kept and
  `event_dropped == 32`;
- map resolution: multi-source OR, unbound action stays 0, deadzone 0 / rescale
  / radial, `dir = -1` flipping a pad axis, `mu_input_axis` sign, `held_time`,
  and repeat cadence (≈15 repeats over 1.2 s at delay 0.30 / rate 20).
- **simple API (v2):** `mu_input_key_down/pressed/released` with identity and
  explicit slot maps (unmapped reads false), `mu_input_mouse_down/pressed/
  released` by `MuMouseButton` enum, tap-inside-one-frame via the key API;
- **wheel single-path + consume:** `wheel_add` queues events (not pre-accumulated),
  `frame_begin` derives `wheel_x/y`, `wheel_consume` is take-once;
- **pad lifecycle:** connect/disconnect events zero buttons/axes, no phantom
  press on reconnect;
- **tick sampler:** `mu_input_sample_tick` + `buffered`/`coyote` exact tick
  boundary, chord fires only on the second press within the window, tick repeat;
- **record/replay:** `mu_input_events` span copied before drain, replayed into
  a fresh state, produces identical held/pressed/wheel;
- **per-source deadzone:** `bind_pad_axis_ex` with explicit deadzone overrides
  the global; slope curve + `sign_f32`; `mouse_move` delta; actions carry
  mouse/wheel (actions-only gameplay).

```sh
gcc -std=c99 -O2 -Wall -Wextra -Werror -Iexternal/mu_lib/mu tests/input.c -o input_test -lm && ./input_test
```

## Evidence: what the live run caught

The demo was driven under `Xvfb` with `xdotool` (real keys, real drag, real
wheel) while the camera state was printed every 0.5 s. Two defects that a
compile-clean, unit-green build did not catch:

1. **Taps were dropped at the action layer.** `resolve` built `down` from held
   bits only, and the tap latch lived one layer below — so a tap that ended
   inside a frame was visible to `mu_input_control_pressed` and invisible to
   `mu_input_action_pressed`. At 8 fps an `xdotool` tap is entirely inside one
   frame, so `P` (pause) did nothing, consistently. Fixed by adding
   `mu_input_source_pressed` (latch-aware) and letting `resolve` set `pressed`
   *and* `released` from taps; the gate test now covers it. This is the exact
   class of bug the header exists to prevent, and it proves the unit test alone
   was not enough.
2. **A blank label.** GLFW reports the Space label as `" "`, so the label
   printer emitted an empty string. Single non-printable characters now fall
   back to the named-key table (`Space`).

Verified through the adapter after both fixes: `D` orbits (`yaw` 0 → 0.36),
`W` zooms in (`distance` 34 → 32.4), right-drag looks (`yaw` −0.32,
`pitch` +0.16), wheel up zooms in (`×0.9`), `P`/`Space` pause and resume,
`-`/`=` change brightness, `R` reseeds, `Esc` exits 0.

## Still missing (roadmap item 2 continues)

- **Shipped in v2:** chords, buffer/coyote in ticks, tick sampler, event-span
  record/replay, pad connect/disconnect, per-source deadzone/curve, simple
  `MuKey`/`MuMouseButton` API. See the API table above.
- **Not yet:** `mu_save.h` chunk serialisation of the event span (the span
  itself is the record format; serialising it is a thin wrapper), IME / text
  entry (deliberately out of scope: `push_*` carries edges, not composed
  text), and per-player device enumeration (per-player *maps* exist as data;
  `pad_connected` + connect events are the seed for enumerating devices).
