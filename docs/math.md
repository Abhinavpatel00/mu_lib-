# Math: noise, geometric algebra, quantization

---

## mu_noise_math.h — procedural noise

Standalone C99/C++ single-header noise library. No allocation, no global
state, no third-party dependencies; link with `-lm`. Every public function is
`MU_INLINE` and takes a `uint32_t seed`.

Supported dimensions vary by family: white/value/perlin cover 1D–4D,
simplex covers 2D–4D, and cellular covers 2D–4D; fractal/warp/curl cover
2D–3D. Periodic, derivative, and batch variants are listed with each family.

### Conventions

- White, value, Perlin, simplex, fBm, billow, and ridged outputs are roughly
  `[-1, 1]`. Turbulence is non-negative. Worley distances are `>= 0`.
- Periodic periods are expressed in lattice cells, not input units.
- `period <= 0` disables periodicity. Functions with multiple periods fall
  back to non-periodic sampling if any required period is non-positive.
- Periodic simplex is natively periodic: it uses an axis-aligned simplex
  lattice so Cartesian axes are period directions. 2D/3D wrap Cartesian corner
  coordinates before hashing; 4D hashes the transformed simplex lattice with
  periodic hash lookups. The tile seam is continuous.
- Lattice interpolation uses quintic smootherstep
  (`6t^5 - 15t^4 + 10t^3`), so first and second derivatives vanish at cell
  boundaries. Domain-warped and cellular fields inherit the smoothness of the
  operators used.
- Finite-difference helpers use central differences and default to
  `eps = 1e-3` when `eps <= 0`. Smaller `eps` reduces smoothing but loses
  precision; larger `eps` is more stable for high-frequency fields.

### Core building blocks

| API | Purpose |
|---|---|
| `mu_noise_hash_u32`, `mu_noise_hash_combine` | integer avalanche hash and seed mixing |
| `mu_noise_hash_{1,2,3,4}i` | lattice hash for integer coordinates |
| `mu_noise_hash_{1,2,3,4}i_periodic` | wrapped lattice hash |
| `mu_noise_hash_to_unit_float` | hash → `[0,1)` |
| `mu_noise_hash_to_signed_float` | hash → `[-1,1]` |
| `mu_noise_smoothstep3`, `mu_noise_smootherstep5` | cubic/quintic interpolation |
| `mu_noise_wrap_period`, `mu_noise_mod_i` | positive and negative period wrapping |
| `mu_noise_remap_neg1_pos1_to_0_1` | signed noise → `[0,1]` |
| `mu_noise_vec2/3/4`, `mu_noise_v2/v3/v4` | small vector types and constructors |
| `mu_noise_v*_dot/len/add/sub/mul/cross/normalize` | vector helpers used by the public API |

### White and lattice noise

| API | Dim | Notes |
|---|---:|---|
| `mu_noise_white1(x, seed)` | 1D | independent random value per integer cell |
| `mu_noise_white2(x, y, seed)` | 2D | |
| `mu_noise_white3(x, y, z, seed)` | 3D | |
| `mu_noise_white4(x, y, z, w, seed)` | 4D | |
| `mu_noise_value1/2/3/4(...)` | 1D–4D | interpolated lattice scalars |
| `mu_noise_value1/2/3/4_periodic(...)` | 1D–4D | tileable value noise |

White noise is discontinuous by design. Value noise is C2-continuous because
the quintic fade is applied per axis.

### Gradient and Perlin noise

| API | Dim | Notes |
|---|---:|---|
| `mu_noise_perlin1(x, seed)` | 1D | gradient noise |
| `mu_noise_perlin2(x, y, seed)` | 2D | |
| `mu_noise_perlin3(x, y, z, seed)` | 3D | |
| `mu_noise_perlin4(x, y, z, w, seed)` | 4D | |
| `mu_noise_perlin1/2/3/4_periodic(...)` | 1D–4D | tileable gradient noise |

Perlin noise stores a pseudo-random gradient at every lattice corner and
interpolates corner dot products. It is cheap and good for terrain, but still
shows some axis alignment.

### Simplex and OpenSimplex-inspired noise

| API | Dim | Notes |
|---|---:|---|
| `mu_noise_simplex2(x, y, seed)` | 2D | 3 corners |
| `mu_noise_simplex3(x, y, z, seed)` | 3D | 4 corners |
| `mu_noise_simplex4(x, y, z, w, seed)` | 4D | 5 corners |
| `mu_noise_simplex2/3/4_periodic(...)` | 2D–4D | native tileable simplex variants |
| `mu_noise_opensimplex2_2d`, `mu_noise_opensimplex2_3d` | 2D/3D | rotated-domain simplex |
| `mu_noise_opensimplex2s_2d`, `mu_noise_opensimplex2s_3d` | 2D/3D | softened two-sample blend |

Simplex noise uses fewer corners than a hypercube and reduces directional
artifacts. The `opensimplex2*` helpers are lightweight approximations intended
for organic fields; they rotate the domain before sampling to break up axis
alignment.

### Analytic derivatives and finite differences

| API | Output |
|---|---|
| `mu_noise_value2_deriv`, `mu_noise_value3_deriv` | `mu_noise_sample2/3` |
| `mu_noise_perlin2_deriv`, `mu_noise_perlin3_deriv` | `mu_noise_sample2/3` |
| `mu_noise_simplex2_deriv`, `mu_noise_simplex3_deriv` | `mu_noise_sample2/3` |
| `mu_noise_value4_deriv`, `mu_noise_perlin4_deriv`, `mu_noise_simplex4_deriv` | `mu_noise_sample4` |

`mu_noise_sample2` contains `value` and `gradient` (`mu_noise_vec2`);
`mu_noise_sample3` contains `value` and `gradient` (`mu_noise_vec3`);
`mu_noise_sample4` contains `value` and `gradient` (`mu_noise_vec4`). The
analytic derivatives are exact for the interpolated field away from the
support cutoffs and are suitable for normals, flow, and gradient-based
effects.

`mu_noise_sample2_fd`, `mu_noise_sample3_fd`, and `mu_noise_sample4_fd`
compute central-difference value+gradient for any callback matching
`mu_noise_fn2`, `mu_noise_fn3`, or `mu_noise_fn4`.
`mu_noise_laplacian2_fd`, `mu_noise_laplacian3_fd`, and
`mu_noise_laplacian4_fd` compute the Laplacian with compact
finite-difference stencils.

### Cellular / Worley noise

```c
mu_noise_worley2_result w =
    mu_noise_worley2(x, y, seed, MU_NOISE_DIST_EUCLIDEAN);
/* w.f1, w.f2, w.cell_id1, w.cell_id2, w.center1, w.center2 */
```

| API | Notes |
|---|---|
| `mu_noise_worley2`, `mu_noise_worley3`, `mu_noise_worley4` | full F1/F2 result |
| `mu_noise_worley2/3/4_periodic` | tileable result |
| `mu_noise_worley2/3/4_f1`, `_f2` | nearest / second-nearest distance |
| `mu_noise_worley2/3/4_edge` | `f2 - f1`, useful for borders/veins |
| `mu_noise_worley2/3/4_cell_id` | hashed ID of nearest cell |
| `mu_noise_worley2/3/4_cell_center` | absolute position of nearest feature point |

Metrics: `MU_NOISE_DIST_EUCLIDEAN`, `MU_NOISE_DIST_MANHATTAN`,
`MU_NOISE_DIST_CHEBYSHEV`. The 2D implementation scans the 3×3 neighborhood,
3D scans 3×3×3, and 4D scans 3×3×3×3. Periodic variants wrap only the hash
lookup, keeping feature centers in unwrapped absolute coordinates so points on
opposite sides of a seam align exactly.

### Fractal compositions

| API | Dim | Formula / use |
|---|---:|---|
| `mu_noise_fbm1/2/3` | 1D–3D | normalized sum of octaves |
| `mu_noise_turbulence1/2/3` | 1D–3D | sum of `|noise|` octaves |
| `mu_noise_billow1/2/3` | 1D–3D | `2|noise| - 1` per octave |
| `mu_noise_ridged1/2/3` | 1D–3D | ridged multifractal with `offset` and gain feedback |
| `mu_noise_multifractal2/3` | 2D–3D | previous octave modulates later amplitudes |
| `mu_noise_hetero_terrain2/3` | 2D–3D | heterogeneous terrain / valley detail |

All fractal functions take a base callback (`mu_noise_fn1/2/3`), coordinates,
seed, `octaves`, `lacunarity`, and `gain`. Ridged functions additionally take
`offset`; multifractal and hetero-terrain also take `offset`. Typical values:
`octaves = 4..8`, `lacunarity = 2.0`, `gain = 0.5`.

```c
float terrain = mu_noise_fbm2(mu_noise_perlin2, x, y, seed, 6, 2.0f, 0.5f);
float clouds  = mu_noise_turbulence2(mu_noise_simplex2, x, y, seed, 5, 2.0f, 0.5f);
float ridges  = mu_noise_ridged2(mu_noise_simplex2, x, y, seed, 5, 2.0f, 2.0f, 1.0f);
```

### Domain warping

| API | Notes |
|---|---|
| `mu_noise_domain_warp2/3` | isotropic vector displacement |
| `mu_noise_domain_warped2/3` | warp then sample a base callback |
| `mu_noise_domain_warp2/3_ex` | anisotropic amplitudes/frequencies |
| `mu_noise_domain_warp2/3_recursive` | repeated warping with `levels` |
| `mu_noise_domain_warped2/3_recursive` | recursive warp + base sample |

Warping computes `p' = p + A * W(f * p)` and then samples the base field at
`p'`. Two or three recursive levels are usually enough; additional levels
become expensive and quickly turn into high-frequency smoke.

### Curl and divergence

| API | Notes |
|---|---|
| `mu_noise_curl2(potential, x, y, seed, eps)` | divergence-free 2D flow from scalar potential |
| `mu_noise_curl3(scalar, x, y, z, seed, eps)` | 3D curl from three seeded scalar components |
| `mu_noise_curl2/3_normalized(...)` | unit-length curl |
| `mu_noise_curl2_magnitude(...)` | length of 2D curl |
| `mu_noise_divergence2/3_fd(field, ...)` | divergence of an arbitrary vector callback |

2D curl uses `v = (dPsi/dy, -dPsi/dx)`, so `div(v) = 0` up to
finite-difference error. 3D curl builds a vector potential from the same scalar
noise with three decorrelated seeds and evaluates `curl(A)` by central
differences. `mu_noise_divergence2_fd` / `mu_noise_divergence3_fd` accept
`mu_noise_vfn2` / `mu_noise_vfn3` callbacks and are useful for validating flow
fields.

### Batch sampling

Struct-of-arrays helpers call a noise callback repeatedly for an array of
coordinates. They are intended for generation loops, not per-sample code.

| API | Purpose |
|---|---|
| `mu_noise_batch1(base, x, out, count, seed)` | 1D callback array |
| `mu_noise_batch2(base, x, y, out, count, seed)` | 2D callback array |
| `mu_noise_batch3(base, x, y, z, out, count, seed)` | 3D callback array |
| `mu_noise_batch4(base, x, y, z, w, out, count, seed)` | 4D callback array |
| `mu_noise_batch2_deriv`, `mu_noise_batch3_deriv`, `mu_noise_batch4_deriv` | arrays of `mu_noise_sample*` |

Supported callback types are `mu_noise_fn1/2/3/4` for scalar noise and
`mu_noise_dfn2/3/4` for derivative samplers. The loops use compiler
vectorization hints where available; for the fastest explicit SIMD path, call
the concrete `mu_noise_value*`, `mu_noise_perlin*`, or `mu_noise_simplex*`
functions directly in your own batch loop so the inliner can remove callback
overhead.

```c
float noise_xy[] = { 0.1f, 1.2f, -3.3f };
float samples[3];

mu_noise_batch2(mu_noise_simplex2, noise_xy, noise_xy + 3, samples, 3, 1337u);
```

### Material and terrain recipes

| API | Output | Typical use |
|---|---|---|
| `mu_noise_terrain_height` | `[-1,1]` | continents plus ridged detail |
| `mu_noise_marble` | `[-1,1]` | sinusoidal marble veins |
| `mu_noise_cells_edge` | `>= 0` | cellular borders |
| `mu_noise_clouds` | `[0,1]` | layered cloud density |
| `mu_noise_wood` | `[0,1]` | warped wood rings + grain |
| `mu_noise_rust` | `[0,1]` | patchy rust mask with pits |
| `mu_noise_caustic` | `[0,1]` | cellular caustic ridges + shimmer |

These are intentionally compact starting points. For production, tune
frequencies/octaves per world scale and combine the result with your own
masks, color ramps, or erosion.

### Usage

```c
float n  = mu_noise_simplex2(12.3f, 9.1f, 1337u);
float fb = mu_noise_fbm2(mu_noise_perlin2, 12.3f, 9.1f,
                         1337u, 6, 2.0f, 0.5f);
float p  = mu_noise_perlin2_periodic(12.3f, 9.1f, 128, 128, 1337u);
float s  = mu_noise_simplex4(1.0f, 2.0f, 3.0f, 4.0f, 1337u);
float sp = mu_noise_simplex4_periodic(1.0f, 2.0f, 3.0f, 4.0f,
                                      16, 16, 16, 16, 1337u);

mu_noise_worley2_result w =
    mu_noise_worley2(12.3f, 9.1f, 1337u, MU_NOISE_DIST_EUCLIDEAN);

mu_noise_sample3 d = mu_noise_simplex3_deriv(1.0f, 2.0f, 3.0f, 1337u);
mu_noise_vec3 normal = mu_noise_v3_normalize(
    mu_noise_v3(-d.gradient.x, -d.gradient.y, 1.0f), 1e-8f);

mu_noise_sample4 d4 = mu_noise_simplex4_deriv(1.0f, 2.0f, 3.0f, 4.0f, 1337u);

Compile as C99 or C++17; include `mu_noise_math.h` directly (it is not part of
the `mu.h` umbrella). Link with `-lm` when using the math functions.

### Choosing a family

| Need | Use |
|---|---|
| Fast uncorrelated jitter | `mu_noise_white1..4` |
| Cheap smooth blobs | `mu_noise_value1..4` |
| Classic terrain texture | `mu_noise_perlin1..4` |
| Fewer axis artifacts | `mu_noise_simplex2..4` / opensimplex helpers |
| Cells, cracks, veins | `mu_noise_worley2/3/4` (`f2 - f1` for borders) |
| Clouds, smoke, erosion | fBm / turbulence / billow / ridged |
| Organic distortion | domain warping |
| Fluid/smoke flow | curl noise |
| Seamless tiles | `*_periodic` variants |
| Normals / gradients | `*_deriv` or finite differences |
| Array generation | `mu_noise_batch*` helpers |

### Performance notes

- White values are one hash; value/Perlin use `2^d` corners; simplex 2D/3D/4D
  uses 3/4/5 corners.
- Periodic simplex is a single simplex evaluation on an axis-aligned lattice,
  not a shifted-sample blend. It costs about the same as the matching
  non-periodic simplex call.
- Worley 2D evaluates 9 cells, Worley 3D 27 cells, Worley 4D 81 cells. Prefer
  F1-only helpers when F2/IDs/centers are not needed.
- Analytic 2D/3D/4D derivatives cost roughly one extra corner pass relative to
  the value call; finite differences cost `2 * dim + 1` samples.
- Fractal cost is `octaves * base_cost`; domain warping multiplies that by the
  number of warp components, and recursive warping multiplies again by
  `levels`.
- Batch helpers are thin loops; they are vectorization-friendly but still pay
  the same per-sample noise cost as the concrete sampler.

---

## mu_geometric_algebra.h — PGA / conformal toolkit

Standalone C99 single-header geometric algebra. **Requires cglm** for the
vector backend (found via `__has_include`, or point `MU_GA_CGLM_HEADER` at
your umbrella header).

Design target:

- Rigid 2D and 3D PGA primitives (flat points, lines, planes)
- 2D/3D **motors** (dual-quaternion-style rigid motion) and **flectors**
  (reflection operators)
- Conformal 2D and 3D primitives (round points, dipoles, circles, spheres)
- Join (wedge) and meet (antiwedge) constructions in Terathon style

### Basis layout (types and their coordinate meaning)

```
Rigid 2D (PGA-like)
    FlatPoint2: p = x e1 + y e2 + z e3
    Line2:      g = x e23 + y e31 + z e12
    Join: p ^ q -> line        Meet: g ^ h -> flat point

Rigid 3D (PGA-like)
    FlatPoint3: p = x e1 + y e2 + z e3 + w e4
    Line3:      l = v_x e41 + v_y e42 + v_z e43 + m_x e23 + m_y e31 + m_z e12
    Plane3:     g = x e234 + y e314 + z e124 + w e321
    Join: p ^ q -> line, line ^ p -> plane
    Meet: g ^ h -> line, g ^ line -> point

Conformal 2D
    RoundPoint2: (x,y,z,w)      Dipole2: line g + flat point p
    Circle2:     (w,x,y,z)

Conformal 3D
    RoundPoint3: (x,y,z,w,u)    Dipole3: (v,m,p)
    Circle3:     (g,v,m)        Sphere3: (u,x,y,z,w)
```

The header is organized into sections: scalar helpers, rigid 2D, rigid 3D,
motor/flector 2D, motor/flector 3D, conformal 2D, conformal 3D, and tiny
usage examples at the bottom. It is intentionally verbose — the math lives
in comments next to the code.

### When this beats matrices

- One type (motor) represents rotation + translation with clean
  composition and interpolation (screw motion / dual-lerp), no gimbal
  worries, no matrix-vs-quaternion split brain.
- Join/meet answer "line through two points", "intersection of two planes",
  "plane through point and line" as single products instead of hand-derived
  cross/dot recipes.
- Reflections are first-class (flectors): mirror transforms compose like any
  other motion.

---

## mu_bitpacking.h — float quantization

Utilities for shrinking float data for rendering: vertex packing, texture
coordinates, normals, GPU bandwidth reduction. All `MU_INLINE`.

### Float bit reinterpretation

```c
typedef union { float f; uint32_t u; } mu_float_bits;
```

Casting `float` to `uint32_t` converts the value; this union (or `memcpy`)
reads the actual IEEE 754 bit pattern — required because quantization
manipulates exponent/mantissa bits directly.

```
float layout:   [ sign | exponent | mantissa ]
                   1        8          23
half layout:    [ sign | exp | mantissa ]
                   1      5       10
```

### API

| Function | Direction | Notes |
|---|---|---|
| `mu_quantize_half(v)` | f32 → f16 | exponent bias adjust (127→15); underflow→0, overflow→inf `0x7c00`, NaN→`0x7e00` |
| `mu_dequantize_half(h)` | f16 → f32 | denormals flushed to zero; inf/NaN preserved |
| `mu_quantize_float(v, n)` | f32 → f32 | keep `n` mantissa bits (0..23) with round-to-nearest; leaves inf/NaN untouched, flushes denormals |
| `mu_quantize_unorm(v, n)` | `[0,1]` → int | n-bit unsigned with rounding; clamps input |
| `mu_dequantize_unorm(v, n)` | int → `[0,1]` | inverse |
| `mu_quantize_snorm(v, n)` | `[-1,1]` → int | n-bit signed, symmetric scale `(1<<(n-1))-1`, sign-aware rounding; clamps input |
| `mu_dequantize_snorm(v, n)` | int → `[-1,1]` | inverse |

### Typical use

```c
/* pack a normal into 10 bits per component */
uint32_t qx = (uint32_t)mu_quantize_snorm(nx, 10);
uint32_t qy = (uint32_t)mu_quantize_snorm(ny, 10);
uint32_t qz = (uint32_t)mu_quantize_snorm(nz, 10);

/* or shrink a float to 10 mantissa bits before storing */
float reduced = mu_quantize_float(1.234567f, 10);
```
