# Math: `mu/mu_math.h` — vectors, quaternions, matrices, SoA batch kernels

Single header, C99/C11, column-major, no allocation, no global state, no
runtime dispatch. Every public function is `MU_INLINE`; `dst` and source may
alias unless a function says otherwise. Include it directly or via `mu.h`.

Its design target is **not** "a faster `glm_mat4_mul`" — cglm's is already a
16-FMA kernel and per-call parity is the realistic floor. It changes the *unit
of work*. The measured evidence, including the rows mu loses, is in
[`perf_test/PERF_MATH.md`](../perf_test/PERF_MATH.md); the gate is
`perf_test/test_math.c` (58676 checks against cglm, four backends).

```sh
make -C perf_test math-test     # correctness gate, non-zero exit on failure
make -C perf_test math-bench    # paired table vs cglm + results_math.csv
```

---

## 1. Layout contract

| type | size / align | notes |
|---|---|---|
| `mu_vec2` | 8 B, 4-aligned | packed |
| `mu_vec3` | **12 B**, 4-aligned | packed — *not* 16. See §5. |
| `mu_vec4` | 16 B, **16-aligned** | |
| `mu_quat` | 16 B | alias of `mu_vec4`; identity `(0,0,0,1)` |
| `mu_mat3` | 36 B | 3 × `mu_vec3` columns |
| `mu_mat4` | 64 B, **64-aligned** | 4 × `mu_vec4` columns, byte-identical to cglm `mat4` and to a GLSL `mat4` upload |
| `mu_plane` | 16 B | alias of `mu_vec4`: `(nx, ny, nz, d)`, positive = inside |
| `mu_frustum` | 96 B | 6 planes: left, right, bottom, top, near, far |
| `mu_mat4_hot` | 64 B | a matrix already in vector registers |
| `mu_mat4x4` | 4 × 512-bit | four matrices, lane-per-matrix (§4) |

`mu_mat4` being byte-identical to cglm's `mat4` is deliberate: interop is a
`memcpy` and `glUniformMatrix4fv(..., (float *)&m)` needs no conversion. The
tests use exactly that as the bridge to the cglm oracle.

Conventions: right-handed, column vectors, `dest = M * v`. `_fast` variants use
hardware rsqrt plus one Newton step (~22 mantissa bits, ~1e-7 relative) — fine
for directions, not for integer-valued money. `affine`/`rigid` mean the last row
is `(0,0,0,1)`; the upper 3×3 is *trusted*, not checked (same trade cglm makes).

---

## 2. API map

### Scalar helpers
`mu_radians`, `mu_degrees`, `mu_lerp_f32`, `mu_clamp_f32`, `mu_sign_f32`,
`mu_inv_sqrt_f32`, `mu_rsqrt_f32`.

### `mu_vec2` / `mu_vec3` / `mu_vec4`
`make`, `splat`, `zero`, `add`, `sub`, `mul` (Hadamard), `scale`, `negate`,
`min`, `max`, `abs`, `lerp`, `muladd` (a·b + c, the one that maps to an FMA),
`dot`, `dot2` (vec3), `cross` (vec3), `length2`, `length`, `distance2`,
`distance`, `normalize`, `normalize_fast`, `reflect`, `project`;
`mu_vec3_to_vec4`, `mu_vec3_from_vec4`.

`mu_vec3_dot`/`mu_vec3_cross` are scalar on purpose: three lanes do not pay for
SIMD and it measures 1.00x against cglm, which is also scalar there.

### `mu_quat`
`identity`, `make`, `axis_angle`, `mul`, `conjugate`, `dot`, `normalize`,
`rotate_vec3`, `slerp`, `nlerp`. `slerp` is transcribed from cglm so the
shortest-arc sign convention matches bit for bit (cglm negates `from`, not `to`;
flipping that silently reverses interpolation).

`rotate_vec3` uses the two-cross form, not `q*v*q*` and not a matrix build:

```c
c  = u x v;              /* u = q.xyz */
v' = v + 2w*c + (2u x c);   /* 2u is precomputed: the factor rides in the
                             * broadcast instead of costing 3 multiplies */
```

Two crosses and three FMAs. **`q` is assumed normalized** — that contract is
where the per-call win against cglm comes from (2.5x when the quaternion varies
per call: `glm_quat_rotatev` re-normalizes every call because it accepts
unnormalized input). When the quaternion is loop-invariant the compiler hoists
cglm's normalize and the two are at parity, so do not quote 2.5x for a loop with
a fixed rotation. For that case use the batch kernel below, which is 4.2x.

### `mu_mat3`
`identity`, `mulv`, `mul`, `from_mat4_linear`, `inverse`, `transpose`,
`inverse_transpose` (the normal matrix — *not* the same as the upper 3×3 when
the transform has non-uniform scale). `inverse` uses
`det = dot(c0, cross(c1, c2))` with column cross products.

### `mu_mat4` — construction
`zero`, `identity`, `from_cols`, `from_quat`, `trs`, `translate`, `scale`,
`rotate`, `perspective`, `perspective_zo` (zero-to-one depth, for D3D/Vulkan/
Metal), `ortho`, `look_at`. `mu_mat4_from_quat` does **not** normalize the
quaternion (documented contract; cglm's `glm_quat_mat4` does, which is why the
bench has a separate `+normalize` row).

### `mu_mat4` — multiply and transform
| function | use |
|---|---|
| `mu_mat4_mul` | general 4×4 · 4×4 |
| `mu_mat4_mul_affine` | 3×4 · 3×4 — 9 FMA + 3 translation terms instead of 16 FMA (2.6x vs `glm_mul`) |
| `mu_mat4_mulv` | matrix · vec4 |
| `mu_mat4_transform_point` / `_dir` | full transform of a point / direction (no perspective divide) |
| `mu_mat4_transform_point_h` | homogeneous result, divide left to the caller |
| `mu_mat4_make_hot`, `mu_mat4_hot_mulv`, `mu_mat4_hot_point` | hoist the 5 loads per call out of a point loop (2.90x vs `glm_mat4_mulv3`) |
| `mu_mat4_transpose`, `_r`, `mu_mat4_determinant` | |

### `mu_mat4` — inverse: pick the right one

| function | contract | vs cglm |
|---|---|---|
| `mu_mat4_inverse` | any invertible 4×4 | **0.85–0.95x** — do not reach for this |
| `mu_inverse_affine` | last row `(0,0,0,1)`, orthogonal columns (axis-aligned scale OK) | 3.44x vs `glm_mat4_inv`, but 0.42x vs `glm_inv_tr` |
| `mu_inverse_rigid` | last row `(0,0,0,1)`, **orthonormal** 3×3 | 0.98x vs `glm_inv_tr`, 8.8x vs `glm_mat4_inv` |

`mu_inverse_rigid` is `[Lᵀ | -Lᵀt]`: a transpose and three dots, no reciprocal.
`mu_inverse_affine` divides each transposed column by its squared length, which
is correct for `R·S` with axis-aligned scale and costs three `rcp`.
`mu_mat4_inverse` is the cofactor/adjugate expansion — one divide, no pivot
branch — and it is the slowest of the three by a wide margin. **If your
matrices are world transforms, the third column of that table is the whole
reason this header has three inverse functions.**

### Frustum
`mu_frustum_from_view_proj`, `mu_frustum_test_sphere` (a plane or the whole
frustum), `mu_frustum_cull_spheres_compact`.
Planes are normalized (Gribb–Hartmann rows of the view-projection), otherwise a
radius test is scale-dependent. `cull_spheres_compact` takes SoA centers and
radii, writes a compacted index list, and returns the surviving count using
AVX-512 compressstores + one `popcount` (28.7x vs a cglm scalar loop on 1M
spheres).

### Batch / SoA — the actual point of the header
| function | what it does |
|---|---|
| `mu_xform_points3_soa` | transform `n` points, SoA `x[]/y[]/z[]` in place, 16 points per 12 FMA with the coefficients live in ZMM registers |
| `mu_xform_normals3_soa` | same for directions (w = 0) |
| `mu_normalize3_soa` | normalize `n` vec3 (2.53x vs cglm on 1M) |
| `mu_quat_rotate_vec3_soa` | rotate `n` vec3 by ONE quaternion, hoisting `u`, `2w`, `2u`; 18 vector ops per 16 points, no shuffles (**4.2x** vs a cglm per-call loop on 1M; in-place safe; masked-free scalar tail) |
| `mu_frustum_cull_spheres_compact` | see above |
| `mu_mat4x4_pack` / `_unpack` / `_mul` | lane-per-matrix pack, multiply, unpack (§4) |
| `mu_mat4_mul_batch` | multiply `n` matrix pairs, with a masked AVX-512 tail so nothing is written past `n` |

Measured, 1M points: **2.65x** with a DRAM-resident stream, **9.01x** when the
working set is L2/L3-resident (64K × 8 passes). Same code; the difference is the
bandwidth roof. A hand-written C loop over the same SoA layout gets the same
~3x, which is the honest reading: the layout and the streaming are the win, not
the intrinsic count.

---

## 3. Backends

`AVX-512F → AVX2+FMA → SSE2 → scalar`, selected entirely by `-march=`. No
runtime dispatch, no IFUNC (AGENTS.md §12.11). `MU_MATH_SCALAR` forces the
portable path on any target, so the scalar reference can be built on the same
box and diffed against the SIMD path — that is how the four-backend gate runs,
and it is what caught a transpose bug in the scalar `mu_mat4_mul`.

All four builds pass the same 58676 checks; ASAN+UBSAN is clean. Because the
contract is "identical semantics per backend", a backend bug is a checksum
mismatch in the bench, not a mystery image.

---

## 4. `mu_mat4x4`: lanes = objects

For a bone palette or instance batch you multiply *unrelated* matrices. Packing
four of them so that each 128-bit lane holds one matrix's column turns four
mat4 muls into 16 FMA + 16 shuffles instead of 4×(16 broadcast + 16 FMA).

```c
mu_mat4x4 pa, pb, pr;
mu_mat4x4_pack(palette, pa);        /* 4 loads + 8 vshuff32x4 */
mu_mat4x4_pack(parents, pb);
mu_mat4x4_mul(&pa, &pb, &pr);       /* 1.7x vs four glm_mat4_mul calls */
mu_mat4x4_unpack(&pr, out);
```

The kernel is 1.7x, but pack+unpack is not free: end-to-end on 256 matrices this
is **1.00x** (parity), and **0.82x** if you re-pack every iteration. Amortise the
pack (pack once per palette update), or use `mu_mat4_mul_batch` instead.

---

## 5. Traps this header has already fallen into

Kept here because they are cheap to re-introduce and expensive to debug.

- **A `mu_vec3` is 12 bytes, not 16.** Passing `mu_vec3[]` to something that
  reads a vec4 (`glm_mat4_mulv`) reads 4 bytes past the last element. Use
  `glm_mat4_mulv3`, or the SoA kernels, or `mu_vec3_to_vec4` per element.
- **`_mm_shuffle_ps(a, b, imm)` takes two lanes from each source.** Several
  cofactor arrangements need a 1/3 split; those are *not* expressible as one
  shuffle. The inverse kernels use `_mm_move_ss` (lane 0) and a
  splat-then-permute (lane 3) for exactly this reason. A 2/2 split that looks
  right on a triangular matrix is wrong on rows 2–3 of a random one.
- **On AVX-512, `mu_mat4_mul` broadcasts A's column and `vpermps`es the single
  64 B load of B.** Doing it the other way round is 4× the loads for the same
  math.
- **`mu_mat4_mul_affine` does not add `t` to columns 0–2.** Column 3 is
  `A · (B.t, 1)`; adding the translation to all four columns is the classic
  affine bug and it looks plausible.
- **Frustum planes must be normalized** or the sphere radius test changes units.
- **`mu_mat4x4_pack` wants a real `mu_mat4[N]`**, not one matrix: indexing a
  single struct out of bounds is `-Werror=array-bounds` waiting to happen.
- **Near-singular input:** mu's cofactor inverse yields `inf` where cglm yields
  `nan`. Neither is a contract; filter on `|det|` when comparing.
- **Do not hand-derive the adjugate sign pattern.** It is transcribed from GLM
  verbatim, and that transcription is the reference the SIMD form is diffed
  against.

---

## 6. What was measured, and where it is not a win

Full tables, methodology and threats to validity: `perf_test/PERF_MATH.md`.
Summary of the rows that do not flatter this header:

| case | ratio | why |
|---|---|---|
| `vec3_normalize_fast` (throughput) | 0.42x | scalar `rsqrt_ss` + Newton cannot be SLP-vectorized; hardware `vsqrtps` can. The "fast" path is 2.4x *slower* per element in a loop. It wins only in a dependent chain (1.06x). |
| `mu_mat4_inverse` | 0.85–0.95x | 76 instructions to cglm's 70; their kernel is already at the floor. |
| `mat3_inverse_transpose` | 0.96x | parity, no story |
| palette, re-packed per iteration | 0.82x | pack cost exceeds the kernel win |
| `mu_inverse_affine` vs `glm_inv_tr` | 0.42x | three unnecessary reciprocals for rigid input → added `mu_inverse_rigid` (0.98x) |

The pattern: **mu wins where the unit of work changes** (streams, batches,
affine/rigid structure) and is at parity or slightly behind on a single object,
which is the regime cglm has spent years optimizing. Any future claim should
carry a row number from `results_math.csv` or it is a placeholder.
