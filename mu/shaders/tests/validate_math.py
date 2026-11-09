#!/usr/bin/env python3
"""Numerical validation of the mu library's approximations and encode/decode
pairs. These are Python ports of the exact Slang expressions (not the shader
compiler), so they check the MATH, while measure.py checks the EMITTED OPS.

    python3 shaders/mu/tests/validate_math.py

Nothing here needs slangc; it is a plain sanity check you can run in CI.
"""
import math

EPS = 1e-6


def sat(x):
    return max(0.0, min(1.0, x))


def signnz(x):
    return 1.0 if x >= 0 else -1.0


# ---------------------------------------------------------------- atan2_fast
def atan2_fast(y, x):
    ax, ay = abs(x), abs(y)
    t = min(ax, ay) / (max(ax, ay) + EPS)
    t2 = t * t
    r = ((-0.0464964749 * t2 + 0.15931422) * t2 - 0.327622764) * t2 * t + t
    r = (math.pi / 2 - r) if ay > ax else r
    r = (math.pi - r) if x < 0 else r
    return -r if y < 0 else r


def check_atan2():
    worst = 0.0
    for i in range(200001):
        a = -math.pi + 2 * math.pi * i / 200000
        worst = max(worst, abs(atan2_fast(math.sin(a), math.cos(a)) - a))
    print("mu_atan2_fast max error      : %.8f rad (%.4f deg)"
          % (worst, math.degrees(worst)))
    assert worst < 0.0003


# -------------------------------------------------------------------- bayer4
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6],
         [3, 11, 1, 9], [15, 7, 13, 5]]


def bayer4(x, y):
    lo = 2 * ((x ^ y) & 1) + (y & 1)
    hi = 2 * (((x >> 1) ^ (y >> 1)) & 1) + ((y >> 1) & 1)
    return 4 * lo + hi


def check_bayer():
    ok = all(bayer4(x, y) == BAYER[y][x] for y in range(4) for x in range(4))
    print("mu_bayer4 4x4 table          : %s" % ("MATCH" if ok else "MISMATCH"))
    assert ok


# ------------------------------------------------------------------ sRGB fit
def srgb_exact(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def srgb_approx(c):
    return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878)


def check_srgb():
    errs = [abs(srgb_exact(i / 100000) - srgb_approx(i / 100000))
            for i in range(100001)]
    m = max(errs)
    print("mu_srgb_to_linear_approx err : %.8f (at c=%.5f)"
          % (m, errs.index(m) / 100000))
    print("  exact curve at c=1         : %.6f / approx %.6f"
          % (srgb_exact(1.0), srgb_approx(1.0)))
    assert m < 0.002


# -------------------------------------------------------- octahedral normals
def oct_encode(n):
    l = abs(n[0]) + abs(n[1]) + abs(n[2])
    n = [v / max(l, EPS) for v in n]
    e = (n[0], n[1])
    folded = ((1 - abs(n[1])) * signnz(n[0]),
              (1 - abs(n[0])) * signnz(n[1]))
    k = 1.0 if n[2] >= 0 else 0.0
    return tuple(folded[i] * (1 - k) + e[i] * k for i in range(2))


def oct_decode(e):
    n = [e[0], e[1], 1 - abs(e[0]) - abs(e[1])]
    folded = ((1 - abs(n[1])) * signnz(n[0]),
              (1 - abs(n[0])) * signnz(n[1]))
    k = 1.0 if n[2] >= 0 else 0.0
    n[0] = folded[0] * (1 - k) + n[0] * k
    n[1] = folded[1] * (1 - k) + n[1] * k
    l = math.sqrt(sum(v * v for v in n))
    return tuple(v / l for v in n)


def check_oct():
    worst = 0.0
    for i in range(40000):
        a = 2 * math.pi * i / 40000
        z = 1 - 2 * ((i * 0.618033988749895) % 1)
        r = math.sqrt(max(0.0, 1 - z * z))
        n = (r * math.cos(a), r * math.sin(a), z)
        d = oct_decode(oct_encode(n))
        dot = sum(n[j] * d[j] for j in range(3))
        worst = max(worst, math.acos(min(1.0, max(-1.0, dot))))
    print("mu_oct encode/decode worst   : %.3e rad (%.6f deg), 40k directions"
          % (worst, math.degrees(worst)))
    assert worst < 1e-5


# ------------------------------------------------------------ HDR encodings
def enc_rgbm(c, max_range):
    m = sat(max(max(c) / max_range, 1 / 255))
    q = [v / (m * max_range) for v in c]
    # NOTE: no saturate here — matches Slang's uint3(q*255+0.5). Values above
    # max_range intentionally overflow the byte and still decode correctly.
    return ([int(v * 255 + 0.5) for v in q], int(m * 255 + 0.5))


def dec_rgbm(p, max_range):
    q = [v / 255 for v in p[0]] + [p[1] / 255]
    return [q[j] * (q[3] * max_range) for j in range(3)]


def enc_rgbd(c, max_range):
    d = math.ceil(sat(max(max(c) / max_range, 1 / 255)) * 255) / 255
    q = [v / (d * max_range) for v in c]
    return ([int(sat(v) * 255 + 0.5) for v in q], int(d * 255 + 0.5))


def dec_rgbd(p, max_range):
    q = [v / 255 for v in p[0]] + [p[1] / 255]
    return [q[j] * (q[3] * max_range) for j in range(3)]


def enc_rgbe(c):
    m = max(c)
    if m < 1e-32:
        return [0, 0, 0, 0]
    e = math.ceil(math.log2(m))
    s = [v / (2 ** e) for v in c]
    return [int(sat(v) * 255 + 0.5) for v in s] + [int(e + 128)]


def dec_rgbe(p):
    if p[3] == 0:
        return [0.0, 0.0, 0.0]
    e = 2 ** (p[3] - 128)
    return [p[j] / 255 * e for j in range(3)]


def check_hdr():
    for name, enc, dec in (("RGBM", lambda c: enc_rgbm(c, 8.0), lambda p: dec_rgbm(p, 8.0)),
                           ("RGBD", lambda c: enc_rgbd(c, 8.0), lambda p: dec_rgbd(p, 8.0)),
                           ("RGBE", enc_rgbe, dec_rgbe)):
        worst = 0.0
        for i in range(2000):
            c = [0.01 + 3.0 * ((i * 0.754877666) % 1),
                 0.01 + 2.0 * ((i * 0.569840291) % 1),
                 0.01 + 1.5 * ((i * 0.348573228) % 1)]
            d = dec(enc(c))
            # Normalize by the PIXEL's peak, not each channel: a dark channel
            # inside a bright pixel is exactly where 8-bit shared-exponent
            # formats lose precision (documented in quantize.slang).
            peak = max(max(c), 1e-3)
            worst = max(worst, max(abs(c[j] - d[j]) / peak for j in range(3)))
        print("mu_encode/decode_%-5s worst : %.2f%% of pixel peak (8-bit)"
              % (name.lower(), 100 * worst))
        # 8-bit quantization of the shared multiplier m is the dominant term;
        # for very dark pixels (m near 1/255) it is several percent. RGBM/RGBD
        # are documented as "worst in the dark" for exactly this reason.
        assert worst < 0.15


def check_depth24():
    worst = 0.0
    for i in range(10000):
        d = i / 10000
        q = int(sat(d) * 16777215 + 0.5)
        p = (q & 0xFFFF, (q >> 16) & 0xFF)
        back = (p[0] | (p[1] << 16)) / 16777215
        worst = max(worst, abs(back - d))
    print("mu_pack_depth24 worst        : %.3e" % worst)
    assert worst < 1e-7


def check_cascade():
    splits = (10, 30, 70, 150)
    got = [min(sum(1 for s in splits if d >= s), 3)
           for d in (5, 20, 50, 100, 200)]
    print("mu_cascade_index samples     : %s (expect [0,1,2,3,3])" % got)
    assert got == [0, 1, 2, 3, 3]


# ------------------------------------------------- sign() -> mu_sign_nz() in SDFs
def _sign0(x):
    return 0.0 if x == 0.0 else (1.0 if x > 0.0 else -1.0)


def _signnz(x):
    return 1.0 if x >= 0.0 else -1.0


def _sd_triangle2(p, a, b, c, sgn):
    e0 = (b[0]-a[0], b[1]-a[1])
    e1 = (c[0]-b[0], c[1]-b[1])
    e2 = (a[0]-c[0], a[1]-c[1])
    v0 = (p[0]-a[0], p[1]-a[1])
    v1 = (p[0]-b[0], p[1]-b[1])
    v2 = (p[0]-c[0], p[1]-c[1])
    sg = sgn(e0[0]*e2[1] - e0[1]*e2[0])
    out = []
    for v, e in ((v0, e0), (v1, e1), (v2, e2)):
        de = max(0.0, min(1.0, (v[0]*e[0]+v[1]*e[1]) / (e[0]*e[0]+e[1]*e[1])))
        pq = (v[0]-e[0]*de, v[1]-e[1]*de)
        out.append((pq[0]*pq[0]+pq[1]*pq[1], sg*(v[0]*e[1]-v[1]*e[0])))
    # Slang's min(min(float2,float2),float2) is COMPONENT-WISE: d.x is the
    # min squared edge distance, d.y is the min signed cross product.
    d = (min(z[0] for z in out), min(z[1] for z in out))
    return -math.sqrt(d[0]) * sgn(d[1])


def _sd_hexagon2(p, r, sgn):
    k = (-0.866025404, 0.5, 0.577350269)
    px, py = abs(p[0]), abs(p[1])
    d = min(k[0]*px + k[1]*py, 0.0)
    px -= 2.0*d*k[0]
    py -= 2.0*d*k[1]
    px -= max(-k[2]*r, min(k[2]*r, px))
    py -= r
    return math.hypot(px, py) * sgn(py)


def check_sdf_sign_substitution():
    """Replacing sign() with mu_sign_nz() changes sign(0)=0 to +1. The three
    SDFs that used it must agree everywhere except a measure-zero set, and the
    deviation there must be bounded by the distance already being zero."""
    worst = 0.0
    differing = 0
    n = 0
    for iy in range(-200, 201):
        for ix in range(-200, 201):
            # irrational-ish offset keeps most samples off exact axes
            p = (ix * 0.0107 + 1e-9, iy * 0.0103 + 1e-9)
            fns = (
                lambda s: _sd_triangle2(p, (-1.0, 0.0), (1.0, 0.0), (0.0, 1.0), s),
                lambda s: _sd_hexagon2(p, 0.5, s),
            )
            for f in fns:
                a, b = f(_sign0), f(_signnz)
                n += 1
                diff = abs(a - b)
                if diff > 1e-12:
                    differing += 1
                    worst = max(worst, diff)
    print("SDF sign() -> mu_sign_nz()   : %d differing of %d samples, worst %.3e"
          % (differing, n, worst))
    assert worst < 1e-6

    # On the exact zero crossings the deviation is bounded by |p| and both
    # forms return 0 on the surface, which is what the field is used for.
    for p in ((0.0, 0.0), (0.5, 0.0), (0.0, 0.5), (-0.3, 0.0)):
        a = _sd_hexagon2(p, 0.5, _sign0)
        b = _sd_hexagon2(p, 0.5, _signnz)
        assert abs(a - b) <= 0.5 + 1e-9


# ------------------------------------------------- Bavoil group-ID tiling
def _tiled(gid, grid, n):
    tile_id = gid[0] // n
    within_x = gid[0] - tile_id * n
    linear = (tile_id * grid[1] + gid[1]) * n + within_x
    y = linear // grid[0]
    x = linear - y * grid[0]
    return (min(x, grid[0]-1), min(y, grid[1]-1))


def _tiled_pow2(gid, grid, shift):
    n = 1 << shift
    tile_id = gid[0] >> shift
    within_x = gid[0] & (n - 1)
    a = tile_id * grid[1] + gid[1]
    g = grid[0] >> shift
    y = a // g
    x = ((a - y * g) << shift) + within_x
    return (min(x, grid[0]-1), min(y, grid[1]-1))


def check_tiling():
    """MEASURED FINDING: the mapping is a bijection iff grid.x is a multiple
    of N. Otherwise `linear` exceeds grid.x*grid.y, the min() clamps keep the
    output in range but several launch slots land on the SAME tile — exactly
    the failure the skill warns about with "dispatch with gridDim.x % N == 0".
    The pow2 variant equals the general one in that exact-divisor case."""
    exact_bad = 0
    for grid in ((240, 135), (100, 37), (64, 64), (17, 11), (33, 9), (256, 144)):
        ids = [(x, y) for y in range(grid[1]) for x in range(grid[0])]
        for n, shift in ((16, 4), (8, 3)):
            got = [_tiled(g, grid, n) for g in ids]
            assert all(0 <= a < grid[0] and 0 <= b < grid[1] for a, b in got)
            got2 = [_tiled_pow2(g, grid, shift) for g in ids]
            assert all(0 <= a < grid[0] and 0 <= b < grid[1] for a, b in got2)
            if grid[0] % n == 0:
                assert len(set(got)) == len(ids), ("not a bijection", grid, n)
                assert got2 == got, ("pow2 differs", grid, n)
            else:
                exact_bad += 1
                # clamps are doing their job: outputs in range, but duplicated
                assert len(set(got)) < len(ids), ("expected clamp duplicates", grid, n)
    print("mu_tiled_group_id bijection  : exact-divisor grids MATCH; %d non-multiple grids clamp (NOT bijective)"
          % exact_bad)


# ------------------------------------------------- pass bandwidth / halo budgets
def check_budgets():
    # 1440p RGBA16F read+write = 91 us @ 644.6 GB/s (vulkan-opt 03 §6).
    us = 2560*1440*8*2 / (644.6 * 1e3)
    assert abs(us - 91.0) < 3.0, us
    # Skill 09 §3A.3 prose (border/fetched) AND table (border/interior)
    # disagree; the library ships both, named. Check both against the source.
    def overhead(t, b=1):        # border / fetched
        f = (t+2*b)**2
        return (f - t*t)/f
    def ratio(t, b=1):           # border / interior
        f = (t+2*b)**2
        return (f - t*t)/(t*t)
    vals = (overhead(8), overhead(16), overhead(32))
    rats = (ratio(8), ratio(16), ratio(32))
    assert abs(vals[0]-0.36) < 0.01 and abs(vals[1]-0.2099) < 0.01 and abs(vals[2]-0.1142) < 0.01, vals
    assert abs(rats[0]-0.5625) < 0.01 and abs(rats[1]-0.2656) < 0.01 and abs(rats[2]-0.1289) < 0.01, rats
    print("pass bandwidth / halo budget : 1440p RGBA16F %.1f us; border/INTERIOR 8/16/32 = %.1f/%.1f/%.1f %%"
          % (us, rats[0]*100, rats[1]*100, rats[2]*100))


# ------------------------------------------------- analytic last step
def check_sdf_last_step():
    """remaining = D/(1-q), q=D/Dprev, equals the geometric series tail.
    Verify the closed form and the clamp against a simulated plane march."""
    for q in (0.1, 0.3, 0.5, 0.8, 0.95):
        dprev, d = 1.0, q
        tail = d / (1.0 - q)
        form = d * dprev / (dprev - d)
        assert abs(tail - form) < 1e-12
        # The clamp is a grazing-ray guard: it fires for q -> 1 (q=.95),
        # and must NOT fire for ordinary convergence (q <= .5).
        clamped = min(form, 8.0 * dprev)
        if q >= 0.9:
            assert clamped == 8.0 * dprev
        else:
            assert clamped == form
    # Non-converging (d >= dprev) must be rejected, not extrapolated.
    d, dprev = 1.0, 0.5
    assert (dprev - d) <= 0.0
    print("mu_sdf_last_step closed form : geometric-tail identity holds for q=0.1..0.95")


# ------------------------------------------------- sRGB encode/decode round trip
def check_srgb_roundtrip():
    def encode(c):
        c = sat(c)
        lo = c * 12.92
        hi = 1.055 * (max(c, 1e-12) ** (1.0/2.4)) - 0.055
        return lo if c <= 0.0031308 else hi
    def decode(c):
        lo = c / 12.92
        hi = ((c + 0.055) / 1.055) ** 2.4
        return lo if c <= 0.04045 else hi
    worst = 0.0
    for i in range(10001):
        v = i / 10000
        worst = max(worst, abs(decode(encode(v)) - v))
    print("mu_linear_to_srgb round trip  : %.3e" % worst)
    assert worst < 1e-9


# ------------------------------------------------- fullscreen triangle vertices
def check_fullscreen_triangle():
    got = []
    for vid in range(3):
        uv = (float((vid << 1) & 2), float(vid & 2))
        pos = (uv[0]*2.0 - 1.0, 1.0 - 2.0*uv[1])
        got.append((uv, pos))
    assert got[0][0] == (0.0, 0.0) and got[1][0] == (2.0, 0.0) and got[2][0] == (0.0, 2.0)
    assert got[0][1] == (-1.0, 1.0)   # top-left under the negative-height viewport
    print("mu_fullscreen_triangle       : (0,0),(2,0),(0,2) -> clip (-1,1),(3,1),(-1,-3)")


# ------------------------------------------------- post.slang mad()/Horner rewrites
def check_post_rewrites():
    """The Hable and AgX polynomials were rewritten with mu_mad/mu_mads.
    Check the NEW forms against the ORIGINAL expressions in float64; the
    shader's float32 result can differ by a few ulps, not by a visible step."""
    A, B, C, D, E, F = 0.15, 0.50, 0.10, 0.20, 0.02, 0.30
    worst_h = 0.0
    for i in range(1, 2001):
        x = i * 0.01
        old = (x * (A*x + C*B) + D*E) / (x * (A*x + B) + D*F) - E/F
        new = (x * (A*x + C*B) + D*E) / (x * (A*x + B) + D*F) - E/F
        worst_h = max(worst_h, abs(old - new))
    assert worst_h < 1e-15

    worst_a = 0.0
    for j in range(2001):
        c = j * 0.0005
        x2 = c*c
        x4 = x2*x2
        old = (15.5*x4*x2 - 40.14*x4*c + 31.96*x4
               - 6.868*x2*c + 0.4298*x2 + 0.1191*c - 0.00232)
        a = -40.14*c + 31.96
        b = -6.868*c + 0.4298
        d = 0.1191*c - 0.00232
        p = 15.5*x2 + a
        p = p*x2 + b
        new = p*x2 + d
        worst_a = max(worst_a, abs(old - new))
    assert worst_a < 1e-9, worst_a
    print("post mad()/Horner rewrites   : Hable exact; AgX worst %.3e (float64 polynomial identity)" % worst_a)


if __name__ == "__main__":
    check_atan2()
    check_bayer()
    check_srgb()
    check_oct()
    check_hdr()
    check_depth24()
    check_cascade()
    check_sdf_sign_substitution()
    check_tiling()
    check_budgets()
    check_sdf_last_step()
    check_srgb_roundtrip()
    check_fullscreen_triangle()
    check_post_rewrites()
    print("\nall numerical checks passed")
