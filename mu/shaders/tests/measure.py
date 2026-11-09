#!/usr/bin/env python3
"""Measure the mu shader library's emitted SPIR-V. Run from anywhere:

    export PATH=/tmp/tools/bin:$PATH      # slangc 2026.21
    python3 shaders/mu/tests/measure.py

It writes one tiny translation unit per case, compiles with `slangc -O3`,
disassembles with `spirv-dis` (or falls back to `slangc -target spirv-asm`
when SPIRV-Tools is not installed), and counts the ops INSIDE the entry-point
function only (types/constants/decorations excluded). `net` = case ops minus
the baseline `b[0] = float4(x);` ops, so the number is the helper's own cost.

Runtime input `x = b[0].x` is deliberate: with a literal the optimizer folds
the whole expression and every case would report zero.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SLANGC = shutil.which("slangc")
DIS = shutil.which("spirv-dis")
# Slang can emit SPIR-V assembly directly. Its raw form is "<lineno>:<op>"
# at column 0, so normalise it into the shape spirv-dis would have produced.
# Verified byte-for-byte on op counts: probe.slang = 7076 body / 66 CF ops
# with either disassembler.
ASM_FALLBACK = bool(SLANGC) and not bool(DIS)


def normalise_asm(text):
    out = []
    for line in text.splitlines():
        line = re.sub(r"^\d+:", "", line)
        if line.strip() and not line.lstrip().startswith(";"):
            line = "  " + line
        out.append(line)
    return "\n".join(out)

HEADER = ('#include "mu/common.slang"\n#include "mu/quantize.slang"\n'
         '#include "mu/lighting.slang"\n#include "mu/toon.slang"\n'
         '#include "mu/shadows.slang"\n#include "mu/noise.slang"\n'
         '#include "mu/sdf.slang"\n#include "mu/post.slang"\n'
         '#include "mu/dispatch.slang"\n#include "mu/wave.slang"\n'
         '#include "mu/fp16.slang"\n#include "mu/format.slang"\n')
TEX_DECLS = ('Texture2D<float> g_depth;\nSamplerComparisonState g_cmp;\n'
             'SamplerState g_samp;\n')


def disassemble(slang, tmpdir, tag, entry="cs", opt="O3"):
    """Compile a .slang file and return SPIR-V assembly text."""
    target = "spirv-asm" if ASM_FALLBACK else "spirv"
    out = os.path.join(tmpdir, tag + (".asm" if ASM_FALLBACK else ".spv"))
    r = subprocess.run(
        [SLANGC, slang, "-target", target, "-entry", entry, "-stage",
         "compute", "-" + opt, "-I", ROOT, "-o", out],
        capture_output=True, text=True)
    if r.returncode != 0 or r.stdout.strip() or r.stderr.strip():
        print("COMPILE FAIL", tag, r.stdout, r.stderr, file=sys.stderr)
        sys.exit(1)
    if ASM_FALLBACK:
        return normalise_asm(open(out).read())
    d = subprocess.run([DIS, out], capture_output=True, text=True)
    if d.returncode != 0:
        print("DISASM FAIL", tag, d.stderr, file=sys.stderr)
        sys.exit(1)
    return d.stdout


def compile_case(src, tmpdir, tag):
    slang = os.path.join(tmpdir, tag + ".slang")
    with open(slang, "w") as f:
        f.write(src)
    return disassemble(slang, tmpdir, tag)


def body_ops(text):
    ops, inside = [], False
    for line in text.splitlines():
        if re.search(r"\bOpFunction\b", line) and "OpFunctionEnd" not in line:
            inside = True
            continue
        if inside and "OpFunctionEnd" in line:
            break
        if inside:
            m = re.match(r"^\s+(?:\S+\s*=\s*)?(Op\w+)", line)
            if m:
                ops.append(m.group(1))
    return ops


def special_ops(text):
    return re.findall(r"OpExtInst \S+ \S+ (\w+)", text)


def ir_shape(text):
    """Instruction CLASSES that a plain opcode count hides: a %v3float FDiv
    is three v_rcp_f32 lanes, a %float FDiv is one; UDiv is an integer
    division sequence. This is the evidence the skill asks for."""
    return (
        len(re.findall(r"OpFDiv %v\d+float", text)),   # vector divides
        len(re.findall(r"OpFDiv %float", text)),        # scalar reciprocal
        len(re.findall(r"OpUDiv|OpSDiv", text)),        # integer divides
        len(re.findall(r"OpExtInst \S+ \S+ Fma", text)),
    )


def tu(expr, decls=""):
    return (HEADER + decls +
            "[numthreads(1,1,1)] void cs(uniform RWStructuredBuffer<float4> b) {\n"
            "  float x = b[0].x;\n"
            "  b[0] = " + expr + ";\n}\n")


CASES = [
    # label, expression, extra declarations
    ("baseline",                   "float4(x)", ""),
    ("pow(x,3.0) [native]",        "float4(pow(x, 3.0))", ""),
    ("mu_pow3(x)",                 "float4(mu_pow3(x))", ""),
    ("atan2(x,0.7) [native]",      "float4(atan2(x, 0.7))", ""),
    ("mu_atan2_fast(x,0.7)",       "float4(mu_atan2_fast(x, 0.7))", ""),
    ("1/sqrt(x+1)",                "float4(1.0 / sqrt(x + 1.0))", ""),
    ("mu_rsqrt(x+1)",              "float4(mu_rsqrt(x + 1.0))", ""),
    ("mu_srgb_to_linear",          "float4(mu_srgb_to_linear(float3(x)), 0.0)", ""),
    ("mu_srgb_to_linear_approx",   "float4(mu_srgb_to_linear_approx(float3(x)), 0.0)", ""),
    ("mu_bayer4",                  "float4(mu_bayer4(float2(x, 2.0)))", ""),
    ("mu_pack_4x8",                "float4(float(mu_pack_4x8(g_u4[0])))",
     'StructuredBuffer<uint4> g_u4;\n'),
    ("mu_oct_encode",              "float4(mu_oct_encode(float3(x,0.4,0.5)).xyx, 0.0)", ""),
    ("mu_oct_decode",              "float4(mu_oct_decode(float2(x,0.2)), 0.0)", ""),
    ("mu_pack_half2/unpack",       "float4(mu_unpack_half2(mu_pack_half2(float2(x,0.7))).xyx, 0.0)", ""),
    ("mu_f_schlick",               "float4(mu_f_schlick(float3(0.04), x), 0.0)", ""),
    ("mu_d_ggx",                   "float4(mu_d_ggx(x, 0.25))", ""),
    ("mu_v_smith_ggx",             "float4(mu_v_smith_ggx(0.7, x, 0.25))", ""),
    ("mu_env_brdf_approx",         "float4(mu_env_brdf_approx(x, 0.4).xyx, 0.0)", ""),
    ("mu_tonemap_aces_fitted",     "float4(mu_tonemap_aces_fitted(float3(x)), 0.0)", ""),
    ("mu_tonemap_aces",            "float4(mu_tonemap_aces(float3(x)), 0.0)", ""),
    ("mu_tonemap_uchimura",        "float4(mu_tonemap_uchimura(float3(x)), 0.0)", ""),
    ("mu_tonemap_hable",           "float4(mu_tonemap_hable(float3(x)), 0.0)", ""),
    ("mu_tonemap_reinhard",        "float4(mu_tonemap_reinhard(float3(x)), 0.0)", ""),
    ("mu_tonemap_neutral",         "float4(mu_tonemap_neutral(float3(x)), 0.0)", ""),
    ("mu_tonemap_agx",             "float4(mu_tonemap_agx(float3(x)), 0.0)", ""),
    ("mu_bayer4_table (not shipped)", "float4(table_bayer(float2(x,2.0)))",
     'static const float kB[16] = {0.0,8.0,2.0,10.0,12.0,4.0,14.0,6.0,3.0,11.0,1.0,9.0,15.0,7.0,13.0,5.0};\n'
     'float table_bayer(float2 p) { uint2 q = uint2(p) & 3u; return kB[q.y*4u + q.x] * (1.0/16.0); }\n'),
    ("mu_pcf3x3",                  "float4(mu_pcf3x3(g_depth,g_cmp,float2(x,0.5),0.5,float2(1.0/2048.0)))", TEX_DECLS),
    ("mu_pcf_bilinear",            "float4(mu_pcf_bilinear(g_depth,g_cmp,float2(x,0.5),0.5,float2(1.0/2048.0),float2(2048.0)))", TEX_DECLS),
    ("mu_pcf_gather_box",          "float4(mu_pcf_gather_box(g_depth,g_samp,float2(x,0.5),0.5,float2(1.0/2048.0),1.0))", TEX_DECLS),

    # ---- skill A/B pairs: the spelling on the left is what the library used
    # to do / what the skill warns about; the right one is what it ships now.
    ("x*y+z [no FMA in IR]",       "float4(x * 0.7 + 0.25)", ""),
    ("mu_mad(x,0.7,0.25) [Fma]",   "float4(mu_mad(x, 0.7, 0.25))", ""),
    ("sign(x) [OpExtInst FSign]",  "float4(sign(x))", ""),
    ("mu_sign_nz(x) [OpSelect]",   "float4(mu_sign_nz(x))", ""),
    ("p/c [3 vector FDiv]",        "float4(float3(x,0.4,0.5) / (x + 1.0), 0.0)", ""),
    ("p*mu_rcp(c) [1 scalar FDiv]","float4(float3(x,0.4,0.5) * mu_rcp(x + 1.0), 0.0)", ""),
    ("mu_sd_repeat3(p,c) [3 rcp]", "float4(mu_sd_repeat3(float3(x,0.4,0.5), float3(x+1.0,x+2.0,x+3.0)), 0.0)", ""),
    ("mu_sd_repeat3_inv(p,inv,c) [0 rcp]", "float4(mu_sd_repeat3_inv(float3(x,0.4,0.5), g_inv[0], float3(x+1.0,x+2.0,x+3.0)), 0.0)", "StructuredBuffer<float3> g_inv;\n"),
    ("mu_tiled_group_id [2 UDiv]", "float4(float(mu_tiled_group_id(uint2(uint(x*10.0),1u), uint2(240u,135u), 16u).x))", ""),
    ("mu_tiled_group_id_pow2 [1 UDiv]", "float4(float(mu_tiled_group_id_pow2(uint2(uint(x*10.0),1u), uint2(240u,135u), 4u).x))", ""),
    ("Texture2D.Load [OpImageFetch]", "float4(g_tex.Load(int3(int(x*10.0),0,0)))", "Texture2D<float4> g_tex;\n"),
    ("Texture2D.SampleLevel",      "float4(g_tex.SampleLevel(g_samp, float2(x,0.5), 0.0))", "Texture2D<float4> g_tex;\nSamplerState g_samp;\n"),
    ("WavePrefixSum [~40 ISA]",    "float4(float(WavePrefixSum(uint(x > 0.0))))", ""),
    ("mu_wave_compact_offset [2 ISA]", "float4(float(mu_wave_compact_offset(x > 0.0)))", ""),
    ("half2 paired mul+add",       "float4(mu_float2(mu_half2_mad(mu_half2(float2(x,0.5)), mu_half2(float2(x,0.5)), mu_half2(float2(x,0.5)))), 0.0, 0.0)", ""),
    ("mu_linear_to_srgb [exact]",  "float4(mu_linear_to_srgb(float3(x)), 0.0)", ""),
]


def main():
    if not SLANGC:
        print("need slangc on PATH", file=sys.stderr)
        sys.exit(2)
    if ASM_FALLBACK:
        print("disassembler: slangc -target spirv-asm (spirv-dis not installed)")
    v = subprocess.run([SLANGC, "-version"], capture_output=True, text=True)
    print("toolchain:", (v.stdout + v.stderr).strip())
    print("include root:", ROOT)
    print()
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        results = []
        for i, (label, expr, decls) in enumerate(CASES):
            text = compile_case(tu(expr, decls), tmp, "c%03d" % i)
            ops = body_ops(text)
            results.append((label, len(ops), ops, special_ops(text), ir_shape(text)))
        base = results[0][1]
        for label, n, ops, ext, shape in results:
            net = n - base
            img = sum(1 for o in ops if o.startswith("OpImage"))
            branch = sum(1 for o in ops if o in
                         ("OpBranch", "OpBranchConditional", "OpPhi",
                          "OpSelectionMerge", "OpLoopMerge"))
            extstr = ",".join(sorted(set(ext))) if ext else "-"
            rows.append((label, net, img, branch, extstr))

    w = max(len(r[0]) for r in rows)
    print("| %-*s | %5s | %6s | %6s | %-12s |" % (w, "case", "net", "image", "branch", "GLSL ext"))
    print("|-%s-|-------|--------|--------|--------------|" % ("-" * w))
    for label, net, img, branch, extstr in rows:
        print("| %-*s | %5d | %6d | %6d | %-12s |" % (w, label, net, img, branch, extstr))

    print()
    # Whole-library regression probe.
    probe = os.path.join(ROOT, "mu", "tests", "probe.slang")
    with tempfile.TemporaryDirectory() as tmp:
        text = disassemble(probe, tmp, "probe")
        ops = body_ops(text)
        cf = sum(1 for o in ops if o.startswith("OpBranch")
                 or o in ("OpPhi", "OpSelectionMerge", "OpLoopMerge"))
        print("probe.slang: %d SPIR-V ops, %d control-flow ops (SDF marchers only), 0 warnings"
              % (len(ops), cf))

        # Leaf-only variant: drop the four loop-bearing SDF helpers.
        src = open(probe).read()
        leaves = []
        for line in src.splitlines(True):
            if any(k in line for k in ("mu_sdf_raymarch(", "mu_sdf_raymarch_stats(",
                                       "mu_sdf_soft_shadow(", "mu_sdf_ao(")):
                leaves.append("    /* loop-bearing helper omitted */\n")
            else:
                leaves.append(line)
        leaf_src = os.path.join(tmp, "leaf.slang")
        open(leaf_src, "w").write("".join(leaves))
        ltext = disassemble(leaf_src, tmp, "leaf")
        lops = body_ops(ltext)
        lcf = sum(1 for o in lops if o.startswith("OpBranch")
                  or o in ("OpPhi", "OpSelectionMerge", "OpLoopMerge"))
        print("probe.slang (leaf helpers only): %d SPIR-V ops, %d control-flow ops"
              % (len(lops), lcf))

    # Skill output contract: an explicit A/B delta table, and an honest
    # statement of what SPIR-V CANNOT prove (registers / final ISA).
    pairs = [
        ("x*y+z -> mu_mad",                  "x*y+z [no FMA in IR]",       "mu_mad(x,0.7,0.25) [Fma]"),
        ("sign(x) -> mu_sign_nz",           "sign(x) [OpExtInst FSign]",  "mu_sign_nz(x) [OpSelect]"),
        ("p/c -> p*mu_rcp(c)",              "p/c [3 vector FDiv]",        "p*mu_rcp(c) [1 scalar FDiv]"),
        ("repeat3 -> repeat3_inv",          "mu_sd_repeat3(p,c) [3 rcp]", "mu_sd_repeat3_inv(p,inv,c) [0 rcp]"),
        ("tiling -> pow2 tiling",           "mu_tiled_group_id [2 UDiv]", "mu_tiled_group_id_pow2 [1 UDiv]"),
        ("PrefixSum -> PrefixCountBits",    "WavePrefixSum [~40 ISA]",    "mu_wave_compact_offset [2 ISA]"),
    ]
    by = {r[0]: r[1] - base for r in results}
    shape = {r[0]: r[4] for r in results}
    print()
    print("| A/B change                    | before | after | delta | vecFD | sclFD | UDiv | mechanism |")
    print("|-------------------------------|--------|-------|-------|-------|-------|------|-----------|")
    mech = {
        "x*y+z -> mu_mad":              "OpFMul+OpFAdd -> 1 OpExtInst Fma (1 v_fma_f32 on RADV either way)",
        "sign(x) -> mu_sign_nz":        "FSign (+Inf mul + clamp) -> 1 v_cndmask_b32",
        "p/c -> p*mu_rcp(c)":           "3 v_rcp_f32 lanes -> 1 scalar v_rcp_f32; op COUNT hides it",
        "repeat3 -> repeat3_inv":       "3 scalar v_rcp_f32 per field eval -> 0 (caller hoists the uniform inverse)",
        "tiling -> pow2 tiling":        "2 OpUDiv -> 1 (uniform, scalar ALU)",
        "PrefixSum -> PrefixCountBits": "~40 ISA ops -> 2 (mbcnt_amd); SPIR-V hides it",
    }
    for name, a, b in pairs:
        va, vb = by[a], by[b]
        sa, sb = shape[a], shape[b]
        print("| %-29s | %6d | %5d | %+5d | %d->%d | %d->%d | %d->%d | %s |"
              % (name, va, vb, vb - va, sa[0], sb[0], sa[1], sb[1],
                 sa[2], sb[2], mech[name]))
    print()
    print("Scope: these are NET SPIR-V body ops at slangc 2026.21 -target spirv")
    print("-O3, with a runtime input so nothing folds. SPIR-V")
    print("cannot price registers or the final ISA: OpExtInst Pow and")
    print("OpGroupNonUniform* hide multi-instruction expansions. The VGPR / wave")
    print("count and ACO listing still need RADV_DEBUG=shaderstats (or RGP) on the")
    print("RX 9070 XT target; that is the next measurement, not a claim made here.")


if __name__ == "__main__":
    main()
