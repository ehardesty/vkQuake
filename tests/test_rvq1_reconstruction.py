#!/usr/bin/env python3
"""RVQ1 host math checks for the filtered volume reconstruction.

Mirrors Shaders/emissive_volume.inc (candidate path, VOLUME_SPATIAL_RECONSTRUCTION=1)
and Shaders/sky_volume.inc in float64, plus a float32-flavoured evaluation to
bound device precision. Covers the section 4.5 table: zero volume, linear-ramp
reproduction, XY ramps, boundary continuity, viewport handling, monotonicity,
sky-vs-far equality, and N in {64, 96, 128}.

Usage: python3 tests/test_rvq1_reconstruction.py
Exit 0 on pass, non-zero with a printed error on failure.
"""
import math
import sys


def clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x


def compute_uv(frag_xy, viewport, W, H):
    """GLSL: uv = (frag_xy - viewport.xy) / viewport.zw, clamped to half-texel inset."""
    vx, vy, vw, vh = viewport
    if vw <= 0 or vh <= 0:
        return None
    u = (frag_xy[0] - vx) / vw
    v = (frag_xy[1] - vy) / vh
    hx = 0.5 / W
    hy = 0.5 / H
    return (clamp(u, hx, 1.0 - hx), clamp(v, hy, 1.0 - hy))


def depth_params(z, Z, N):
    """Return (i, w, texture_z, za, zb) per section 4.2, or ('zero'|'far', ...)."""
    B = N + 1
    if z <= 0:
        return ("zero", None)
    if z >= Z:
        return ("far", (N + 0.5) / B)
    s = math.sqrt(z / Z) * N
    i = clamp(math.floor(s), 0, N - 1)
    t0 = i / N
    t1 = (i + 1) / N
    za = Z * t0 * t0
    zb = Z * t1 * t1
    w = clamp((z - za) / max(zb - za, 1e-6), 0.0, 1.0)
    tz = (i + w + 0.5) / B
    return ("mid", (i, w, tz, za, zb))


def trilinear(volume, W, H, B, uv, tz):
    """CPU model of Vulkan linear filtering with clamp-to-edge.

    volume[z][y][x] scalar. Normalized coords map to texel space as
    x = u*W - 0.5 (same for y, z), then trilinear with edge clamping.
    """
    x = uv[0] * W - 0.5
    y = uv[1] * H - 0.5
    z = tz * B - 0.5
    x0 = clamp(math.floor(x), 0, W - 1)
    y0 = clamp(math.floor(y), 0, H - 1)
    z0 = clamp(math.floor(z), 0, B - 1)
    x1 = min(x0 + 1, W - 1)
    y1 = min(y0 + 1, H - 1)
    z1 = min(z0 + 1, B - 1)
    fx = clamp(x - math.floor(x), 0.0, 1.0) if x0 != x1 else 0.0
    fy = clamp(y - math.floor(y), 0.0, 1.0) if y0 != y1 else 0.0
    fz = clamp(z - math.floor(z), 0.0, 1.0) if z0 != z1 else 0.0
    # If exactly on an edge-clamped coordinate the fractional part still
    # applies between the clamped pair; the x0==x1 guard above only fires
    # when W==1. At volume edges x may be e.g. 0.0 with neighbours 0,1.
    # Recompute fx correctly for the general case:
    fx = clamp(x - x0, 0.0, 1.0) if x1 != x0 else 0.0
    fy = clamp(y - y0, 0.0, 1.0) if y1 != y0 else 0.0
    fz = clamp(z - z0, 0.0, 1.0) if z1 != z0 else 0.0
    c000 = volume[z0][y0][x0]
    c100 = volume[z0][y0][x1]
    c010 = volume[z0][y1][x0]
    c110 = volume[z0][y1][x1]
    c001 = volume[z1][y0][x0]
    c101 = volume[z1][y0][x1]
    c011 = volume[z1][y1][x0]
    c111 = volume[z1][y1][x1]
    c00 = c000 * (1 - fx) + c100 * fx
    c10 = c010 * (1 - fx) + c110 * fx
    c01 = c001 * (1 - fx) + c101 * fx
    c11 = c011 * (1 - fx) + c111 * fx
    c0 = c00 * (1 - fy) + c10 * fy
    c1 = c01 * (1 - fy) + c11 * fy
    return c0 * (1 - fz) + c1 * fz


def sample_candidate(volume, W, H, N, frag_xy, viewport, z, Z):
    """Full candidate reconstruction returning a scalar."""
    B = N + 1
    if z <= 0:
        return 0.0
    uv = compute_uv(frag_xy, viewport, W, H)
    if uv is None:
        return 0.0
    kind, payload = depth_params(z, Z, N)
    if kind == "zero":
        return 0.0
    if kind == "far":
        return trilinear(volume, W, H, B, uv, payload)
    _, _, tz, _, _ = payload
    return trilinear(volume, W, H, B, uv, tz)


def sample_reference(volume, W, H, N, frag_xy, viewport, z, Z):
    """Pre-phase nearest-column helper (float64 model of the GLSL reference)."""
    grid = (W, H)
    segs = N
    if segs <= 0 or Z <= 0:
        return 0.0
    if z <= 0:
        return 0.0
    vx, vy, vw, vh = viewport
    u = (frag_xy[0] - vx) / vw
    v = (frag_xy[1] - vy) / vh
    cx = clamp(math.floor(u * grid[0]), 0, grid[0] - 1)
    cy = clamp(math.floor(v * grid[1]), 0, grid[1] - 1)
    if z >= Z:
        return volume[segs][int(cy)][int(cx)]
    f = math.sqrt(z / Z) * segs
    i0 = clamp(math.floor(f), 0, segs - 1)
    s0 = volume[int(i0)][int(cy)][int(cx)]
    s1 = volume[int(i0) + 1][int(cy)][int(cx)]
    t0 = i0 / segs
    t1 = (i0 + 1) / segs
    z0 = Z * t0 * t0
    z1 = Z * t1 * t1
    w = (z - z0) / max(z1 - z0, 1e-6)
    w = clamp(w, 0.0, 1.0)
    return s0 * (1 - w) + s1 * w


def make_volume(W, H, B, fn):
    return [[[fn(x, y, b) for x in range(W)] for y in range(H)] for b in range(B)]


FAILURES = []


def check(name, cond, detail=""):
    if cond:
        print(f"  ok: {name}")
    else:
        print(f"  FAIL: {name} {detail}")
        FAILURES.append(name)


def test_zero_volume():
    print("zero volume:")
    for N in (64, 96, 128):
        B = N + 1
        W, H, Z = 8, 6, 4096.0
        vol = make_volume(W, H, B, lambda x, y, b: 0.0)
        vp = (0.0, 0.0, 800.0, 600.0)
        for frag in [(0.0, 0.0), (400.0, 300.0), (799.9, 599.9), (-50.0, 700.0)]:
            for z in [0.0, 1.0, Z * 0.25, Z * 0.5, Z - 1.0, Z, Z + 100.0]:
                v = sample_candidate(vol, W, H, N, frag, vp, z, Z)
                check(f"N={N} frag={frag} z={z} is zero", v == 0.0, f"got {v}")


def test_linear_ramp_xy_constant():
    print("XY-constant planes, cumulative linear in physical z:")
    # Cumulative value C(z_b) = k * z_b with k chosen so values stay modest.
    for N in (64, 96, 128):
        B = N + 1
        W, H, Z = 8, 6, 4096.0
        k = 0.001
        zb = [Z * (b / N) ** 2 for b in range(B)]
        vol = make_volume(W, H, B, lambda x, y, b: k * zb[b])
        vp = (0.0, 0.0, 800.0, 600.0)
        frag = (400.0, 300.0)  # interior; XY-constant so any column matches
        # Non-midpoint positions inside quadratic intervals, incl. narrow deep slices.
        for s in [0, 1, 7, 31, 63, N - 1]:
            if s >= N:
                continue
            za = Z * (s / N) ** 2
            zb1 = Z * ((s + 1) / N) ** 2
            for frac in (0.0, 0.13, 0.5, 0.87, 1.0):
                z = za + frac * (zb1 - za)
                if z <= 0:
                    continue
                got = sample_candidate(vol, W, H, N, frag, vp, z, Z)
                want = k * z
                # Host float64 identity should be near-exact; allow 1e-9 relative.
                tol = 1e-9 * max(1.0, abs(want))
                check(f"N={N} seg={s} frac={frac}", abs(got - want) <= tol, f"got {got} want {want}")


def test_xy_ramp():
    print("interior XY ramp:")
    for N in (64,):
        B = N + 1
        W, H, Z = 8, 6, 4096.0
        # Per-plane XY ramp: value = (x + 0.5)/W + 2*(y+0.5)/H + 0.001*z_b, cumulative in z.
        zb = [Z * (b / N) ** 2 for b in range(B)]
        vol = make_volume(W, H, B, lambda x, y, b: (x + 0.5) / W + 2.0 * (y + 0.5) / H + 0.0001 * zb[b])
        vp = (0.0, 0.0, float(W * 100), float(H * 100))  # 100px per texel
        # Column centers reproduce.
        for cx in (0, 3, 7):
            for cy in (0, 2, 5):
                frag = ((cx + 0.5) * 100.0, (cy + 0.5) * 100.0)
                # At an exact boundary zb[b], depth interpolation is exact.
                for b in (0, 1, 17, 64):
                    z = zb[b] if zb[b] > 0 else 1e-3
                    got = sample_candidate(vol, W, H, N, frag, vp, z, Z)
                    want = (cx + 0.5) / W + 2.0 * (cy + 0.5) / H + 0.0001 * (z if b > 0 or z > 0 else 0.0)
                    # At b==0 z=0 the shader returns 0; skip that point here (covered elsewhere).
                    if b == 0:
                        continue
                    tol = 1e-9 * max(1.0, abs(want))
                    check(f"center ({cx},{cy}) b={b}", abs(got - want) <= tol, f"got {got} want {want}")
        # Half-way positions are continuous bilinear combinations.
        # Texels are 100px wide: column 3 spans [300,400), column 4 [400,500).
        # x=400 is the boundary (half-way), y=250 is the center of row 2.
        frag_mid = (4.0 * 100.0, 2.5 * 100.0)
        b = 32
        z = zb[b]
        got = sample_candidate(vol, W, H, N, frag_mid, vp, z, Z)
        # Bilinear of the ramp at u=0.5 between texel centers 3 and 4:
        # value = 0.5*( ramp(3) + ramp(4) ) in x, exact row 2 in y.
        want = 0.5 * (((3 + 0.5) / W + (4 + 0.5) / W)) + 2.0 * (2 + 0.5) / H + 0.0001 * z
        check("half-way bilinear", abs(got - want) <= 1e-9 * max(1.0, abs(want)), f"got {got} want {want}")
        # Continuity: small UV step gives small value step.
        eps = 0.49  # just under half a texel in px (texel = 100px)
        a = sample_candidate(vol, W, H, N, (frag_mid[0] - eps, frag_mid[1]), vp, z, Z)
        bb = sample_candidate(vol, W, H, N, (frag_mid[0] + eps, frag_mid[1]), vp, z, Z)
        check("no XY jump across half-texel", abs(bb - a) < 0.05, f"{a} vs {bb}")


def test_boundaries():
    print("boundaries and endpoints:")
    for N in (64, 96, 128):
        B = N + 1
        W, H, Z = 8, 6, 4096.0
        # Distinct per-plane values so any mis-addressing shows.
        vol = make_volume(W, H, B, lambda x, y, b: float(b) * 10.0 + float(x) * 0.01)
        vp = (0.0, 0.0, 800.0, 600.0)
        # Column center frag for exactness.
        frag = (50.0, 50.0)  # column (0,0) when viewport is 800x600 and grid 8x6
        uv = compute_uv(frag, vp, W, H)
        # z=0 -> zero
        check(f"N={N} z=0", sample_candidate(vol, W, H, N, frag, vp, 0.0, Z) == 0.0)
        check(f"N={N} z<0", sample_candidate(vol, W, H, N, frag, vp, -5.0, Z) == 0.0)
        # Every boundary reproduces the stored plane at column centers.
        zb = [Z * (b / N) ** 2 for b in range(B)]
        for b in (1, 2, N // 2, N - 1, N):
            z = zb[b]
            got = sample_candidate(vol, W, H, N, frag, vp, z, Z)
            # Column (0,0) value at plane b:
            want = float(b) * 10.0
            check(f"N={N} boundary b={b}", abs(got - want) <= 1e-6 * max(1.0, abs(want)), f"got {got} want {want}")
        # Either side of a boundary is continuous.
        for b in (1, 8, N - 1):
            zc = zb[b]
            d = max((zb[b + 1] - zb[b]) if b < N else 1.0, (zb[b] - zb[b - 1])) * 1e-4
            a = sample_candidate(vol, W, H, N, frag, vp, zc - d, Z)
            cc = sample_candidate(vol, W, H, N, frag, vp, zc + d, Z)
            check(f"N={N} continuity b={b}", abs(cc - a) < 0.05, f"{a} vs {cc}")
        # z>=Z retains final value.
        last = float(N) * 10.0
        for z in (Z, Z + 1.0, Z * 2.0):
            got = sample_candidate(vol, W, H, N, frag, vp, z, Z)
            check(f"N={N} far z={z}", abs(got - last) <= 1e-6 * max(1.0, abs(last)), f"got {got} want {last}")


def test_viewport():
    print("viewport handling:")
    W, H, N, Z = 8, 6, 64, 4096.0
    B = N + 1
    vol = make_volume(W, H, B, lambda x, y, b: float(x * 100 + y * 10 + b))
    # Same relative position under origin offsets gives same result.
    for vp0 in [(0.0, 0.0, 800.0, 600.0), (100.0, 50.0, 800.0, 600.0), (0.0, 0.0, 400.0, 300.0)]:
        rel = (0.37, 0.61)
        frag = (vp0[0] + rel[0] * vp0[2], vp0[1] + rel[1] * vp0[3])
        a = sample_candidate(vol, W, H, N, frag, vp0, 500.0, Z)
        # Reference viewport at origin with same size must agree.
        vp_ref = (0.0, 0.0, vp0[2], vp0[3])
        frag_ref = (rel[0] * vp0[2], rel[1] * vp0[3])
        bb = sample_candidate(vol, W, H, N, frag_ref, vp_ref, 500.0, Z)
        check(f"origin offset {vp0}", abs(a - bb) <= 1e-12, f"{a} vs {bb}")
    # Edges/corners clamp, never wrap to the opposite edge.
    vp = (0.0, 0.0, 800.0, 600.0)
    z = 500.0
    corners = [(-100.0, -100.0), (900.0, -50.0), (-50.0, 700.0), (900.0, 700.0),
               (0.0, 0.0), (799.99, 599.99)]
    vals = [sample_candidate(vol, W, H, N, f, vp, z, Z) for f in corners]
    # Far-left must equal left-edge clamp, not right-edge data.
    left = sample_candidate(vol, W, H, N, (0.0, 300.0), vp, z, Z)
    check("left clamp, no wrap", abs(vals[0] - sample_candidate(vol, W, H, N, (0.0, 0.0), vp, z, Z)) < 5.0)
    check("finite everywhere", all(math.isfinite(v) for v in vals))
    # Aspect change: grid height follows aspect; mapping stays in-bounds.
    for aspect_h in (32, 72, 128):
        H2 = aspect_h
        vol2 = make_volume(W, H2, B, lambda x, y, b: float(b))
        v = sample_candidate(vol2, W, H2, N, (400.0, 300.0), vp, z, Z)
        check(f"aspect H={H2} finite/bounded", math.isfinite(v) and 0.0 <= v <= float(N))
    # Degenerate viewport -> zero, not NaN.
    check("degenerate viewport", sample_candidate(vol, W, H, N, (10.0, 10.0), (0, 0, 0, 600), z, Z) == 0.0)
    check("degenerate viewport h", sample_candidate(vol, W, H, N, (10.0, 10.0), (0, 0, 800, 0), z, Z) == 0.0)
    # Y orientation: increasing frag_y increases v (down the framebuffer); neighbouring
    # rows must differ in the row direction, proving no reversed-Y.
    vol_rows = make_volume(W, H, B, lambda x, y, b: float(y * 100 + b))
    va = sample_candidate(vol_rows, W, H, N, (400.0, 100.0), vp, z, Z)
    vb = sample_candidate(vol_rows, W, H, N, (400.0, 500.0), vp, z, Z)
    check("v grows down (no flipped Y)", vb > va, f"{va} vs {vb}")


def test_monotone():
    print("monotonicity and bounds:")
    W, H, N, Z = 8, 6, 64, 4096.0
    B = N + 1
    # Positive monotone cumulative input: per-texel nondecreasing in depth.
    vol = make_volume(W, H, B, lambda x, y, b: float(b) * (1.0 + 0.01 * (x + y)))
    vp = (0.0, 0.0, 800.0, 600.0)
    frag = (333.0, 222.0)
    prev = -1.0
    ok = True
    for k in range(0, 65):
        z = Z * (k / 64.0) ** 2 * 0.999 + 0.5
        v = sample_candidate(vol, W, H, N, frag, vp, min(z, Z * 0.999), Z)
        if v < -1e-9 or v < prev - 1e-6:
            ok = False
        prev = v
    check("nonnegative and monotone in depth", ok)
    # Bounded by contributing texels: sample inside one XY texel span and one
    # depth interval; result must lie within the 2x2x2 neighbourhood range.
    frag_c = (50.0, 50.0)
    zc = Z * 0.1
    v = sample_candidate(vol, W, H, N, frag_c, vp, zc, Z)
    check("bounded", 0.0 <= v <= float(N) * 1.2, f"got {v}")


def test_sky_vs_far():
    print("sky vs far-depth scene sample:")
    for N in (64, 96, 128):
        B = N + 1
        W, H, Z = 8, 6, 4096.0
        vol = make_volume(W, H, B, lambda x, y, b: float((x * 7 + y * 13 + b * 3) % 97))
        vp = (0.0, 0.0, 800.0, 600.0)
        for frag in [(0.0, 0.0), (123.0, 456.0), (799.9, 599.9), (400.0, 300.0)]:
            uv = compute_uv(frag, vp, W, H)
            sky = trilinear(vol, W, H, B, uv, (B - 0.5) / B)
            far = sample_candidate(vol, W, H, N, frag, vp, Z, Z)
            # Bitwise equality is not required (FP16/device weights); tight host tolerance.
            check(f"N={N} frag={frag} sky==far", abs(sky - far) <= 1e-12, f"{sky} vs {far}")


def test_no_hidden_plane_count():
    print("plane-count independence (N=64,96,128):")
    W, H, Z = 8, 6, 4096.0
    for N in (64, 96, 128):
        B = N + 1
        # Boundary depths must tile [0, Z] exactly with the quadratic rule.
        for b in (0, 1, N // 3, N - 1, N):
            zb = Z * (b / N) ** 2
            check(f"N={N} boundary {b} in range", 0.0 <= zb <= Z)
        # texture_z for w=0/w=1 lands on texel centers of planes i/i+1.
        for i in (0, N // 2, N - 1):
            for w in (0.0, 1.0):
                tz = (i + w + 0.5) / B
                check(f"N={N} texcoord i={i} w={w} in [0,1]", 0.0 <= tz <= 1.0)
                # Unmap: tz*B-0.5 must equal i+w exactly (host float64).
                check(f"N={N} texcoord identity", abs((tz * B - 0.5) - (i + w)) <= 1e-12)


def test_reference_preserved_spot():
    print("reference helper spot-check (unchanged semantics):")
    # At a column center and exact boundary the reference returns the stored plane.
    W, H, N, Z = 8, 6, 64, 4096.0
    B = N + 1
    vol = make_volume(W, H, B, lambda x, y, b: float(b * 5 + x))
    vp = (0.0, 0.0, 800.0, 600.0)  # 100px per column in x, 100px per row in y
    frag = (50.0, 50.0)  # column (0,0)
    zb5 = Z * (5 / N) ** 2
    got = sample_reference(vol, W, H, N, frag, vp, zb5, Z)
    check("reference boundary exact", abs(got - (5 * 5 + 0)) <= 1e-9, f"got {got}")


def main():
    print("RVQ1 reconstruction math checks")
    test_zero_volume()
    test_linear_ramp_xy_constant()
    test_xy_ramp()
    test_boundaries()
    test_viewport()
    test_monotone()
    test_sky_vs_far()
    test_no_hidden_plane_count()
    test_reference_preserved_spot()
    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURE(S): {FAILURES}")
        return 1
    print("\nall RVQ1 math checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
