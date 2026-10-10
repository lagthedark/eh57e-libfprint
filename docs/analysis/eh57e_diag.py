#!/usr/bin/env python3
"""EH57E matcher diagnostics — faithful numpy port of egis057e-matcher.c.

Measures:
  D1: ridge period of real captures (FFT radial + autocorr ring)
  D2: the FIR bank's DESIGN ridge period via synthetic gratings
  D3: fine scale sweep on the real same-finger pair (score/votes/features)
  D4: at best scale: genuine vs control pairs, NN-Hamming distributions,
      IP repeatability, orientation agreement
"""
import numpy as np, math, sys

np.set_printoptions(suppress=True)

# ---------------- exact constants from egis057e-matcher.{c,h} ----------------
FIR = np.array([
 [1785,8003,13193,8002,1785,0,0,0,0,0,0],
 [2319,8011,12109,8010,2319,0,0,0,0,0,0],
 [2806,7951,11253,7952,2806,0,0,0,0,0,0],
 [706,3097,7524,10114,7524,3097,706,0,0,0,0],
 [950,3402,7313,9438,7313,3402,950,0,0,0,0],
 [1200,3646,7104,8870,7102,3646,1200,0,0,0,0],
 [361,1415,3757,6748,8205,6749,3757,1415,361,0,0],
 [486,1632,3877,6517,7746,6515,3877,1632,486,0,0],
 [622,1832,3962,6295,7346,6295,3962,1832,622,0,0],
 [216,753,1985,3967,6011,6904,6011,3967,1985,753,216],
 [289,889,2134,3986,5800,6572,5800,3986,2134,889,289]], float)
FIR_LEN = [5,5,5,7,7,7,9,9,9,11,11]
W_ROW = np.array([57,118,207,308,392,425,392,308,207,118,57], float)
W_COL = np.array([53,109,191,285,362,392,362,285,191,109,53], float)
R_FLAT, MARGIN, NMS_R2, MAXF = 7, 6, 36, 120
DESC_HALF, REGIONS, BINS = 5, 4, 32
BINW = 360.0 / BINS
BUDGET, MINM, POSTOL, ANGTOL, BUCKET = 115, 5, 8, 12.0, 8
N_OR = 11

# ---------------- PGM ----------------
def load_pgm(path):
    with open(path, 'rb') as f:
        data = f.read()
    # tokenize header allowing comments
    toks, i = [], 0
    while len(toks) < 4:
        # skip whitespace
        while i < len(data) and data[i:i+1].isspace(): i += 1
        if data[i:i+1] == b'#':
            while i < len(data) and data[i:i+1] != b'\n': i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace(): j += 1
        toks.append(data[i:j]); i = j
    assert toks[0] == b'P5', toks[0]
    w, h, mx = int(toks[1]), int(toks[2]), int(toks[3])
    assert mx == 255
    i += 1  # single whitespace
    img = np.frombuffer(data, np.uint8, w*h, i).reshape(h, w).copy()
    return img

def resize_bilinear(img, s):
    h, w = img.shape
    H, W = int(round(h*s)), int(round(w*s))
    ys = (np.arange(H) + 0.5) / s - 0.5
    xs = (np.arange(W) + 0.5) / s - 0.5
    ys = np.clip(ys, 0, h-1); xs = np.clip(xs, 0, w-1)
    yi = np.floor(ys).astype(int); xi = np.floor(xs).astype(int)
    yi = np.clip(yi, 0, h-2); xi = np.clip(xi, 0, w-2)
    fy = ys - np.clip(np.floor(ys), 0, h-2); fx = xs - np.clip(np.floor(xs), 0, w-2)
    # careful: replicate C clamp_sample semantics loosely; standard bilinear fine here
    A = img[np.ix_(yi, xi)];     B = img[np.ix_(yi, xi+1)]
    C = img[np.ix_(yi+1, xi)];   D = img[np.ix_(yi+1, xi+1)]
    fy = (fy[:,None]); fx = fx[None,:]
    return A*(1-fy)*(1-fx) + B*(1-fy)*fx + C*fy*(1-fx) + D*fy*fx

# ---------------- pipeline port ----------------
def flat_field(img):
    h, w = img.shape
    R = R_FLAT
    cs = np.zeros((h+1, w+1), np.float64)
    ii = img.astype(np.float64)
    cs[1:,1:] = ii
    cs = np.cumsum(np.cumsum(cs, 0), 1)
    out = np.empty((h, w))
    for y in range(h):
        y0, y1 = y-R, y+R+1
        ya, yb = max(y0,0), min(y1,h)
        for x in range(w):
            x0, x1 = x-R, x+R+1
            xa, xb = max(x0,0), min(x1,w)
            cnt = (xb-xa)*(yb-ya)
            s = cs[yb,xb] - cs[ya,xb] - cs[yb,xa] + cs[ya,xa]
            out[y,x] = ii[y,x] - s/cnt
    n = h*w
    std = math.sqrt(max(out.var(), 0.0))
    return out / (std + 1e-9)

def clamp_sample(img, SX, SY):
    """Vectorized C clamp_sample. SX,SY float arrays."""
    h, w = img.shape
    xi = np.trunc(SX).astype(np.int64); fx = SX - xi
    m = xi < 0;  xi[m] = 0; fx[m] = 0.0
    m = xi > w-2; xi[m] = w-2; fx[m] = 1.0
    yi = np.trunc(SY).astype(np.int64); fy = SY - yi
    m = yi < 0;  yi[m] = 0; fy[m] = 0.0
    m = yi > h-2; yi[m] = h-2; fy[m] = 1.0
    a = img[yi, xi]; b = img[yi, xi+1]; c = img[yi+1, xi]; d = img[yi+1, xi+1]
    return a*(1-fy)*(1-fx) + b*(1-fy)*fx + c*fy*(1-fx) + d*fy*fx

def oriented_bank(flat, stack=False):
    h, w = flat.shape
    enh = np.full((h, w), -1e18)
    oidx = np.zeros((h, w), np.uint8)
    ys, xs = np.mgrid[0:h, 0:w]
    ys = ys.astype(np.float64); xs = xs.astype(np.float64)
    stacks = np.empty((N_OR, h, w)) if stack else None
    for o in range(N_OR):
        theta = 180.0*o/N_OR
        rad = theta*math.pi/180.0
        cs_, sn_ = math.cos(rad), math.sin(rad)
        L = FIR_LEN[o]; c = L//2
        k = FIR[o, :L] / FIR[o, :L].sum()
        acc = np.zeros((h, w))
        for t in range(L):
            d = t - c
            acc += k[t] * clamp_sample(flat, xs + d*cs_, ys - d*sn_)
        if stack: stacks[o] = acc
        a = np.abs(acc)
        upd = a > enh
        enh[upd] = a[upd]; oidx[upd] = o
    return (enh, oidx, stacks) if stack else (enh, oidx)

def gradients(enh):
    h, w = enh.shape
    gx = np.zeros_like(enh); gy = np.zeros_like(enh)
    gx[:,1:-1] = (enh[:,2:] - enh[:,:-2]) / 2.0
    gx[:,0] = enh[:,1] - enh[:,0]; gx[:,-1] = enh[:,-1] - enh[:,-2]
    gy[1:-1,:] = (enh[2:,:] - enh[:-2,:]) / 2.0
    gy[0,:] = enh[1,:] - enh[0,:]; gy[-1,:] = enh[-1,:] - enh[-2,:]
    return gx, gy

def candidates(enh):
    """Strict 3x3 maxima with the C tie rule; returns x,y,val arrays (scan order)."""
    h, w = enh.shape
    v = enh[MARGIN:h-MARGIN, MARGIN:w-MARGIN]
    inf = 1e30
    def sh(dy, dx):
        return enh[MARGIN+dy:h-MARGIN+dy, MARGIN+dx:w-MARGIN+dx]
    nbr_max = np.full_like(v, -np.inf)
    for dy in (-1,0,1):
        for dx in (-1,0,1):
            if dy == 0 and dx == 0: continue
            nbr_max = np.maximum(nbr_max, sh(dy,dx))
    keep = v >= nbr_max  # v is max of 3x3
    # earlier-scan-order equal neighbours reject
    for dy, dx in ((-1,-1),(-1,0),(-1,1),(0,-1)):
        keep &= ~(sh(dy,dx) == v)
    ys, xs = np.nonzero(keep)
    return xs + MARGIN, ys + MARGIN, v[ys, xs]

def descriptors(y, x, o, gx, gy):
    """One 512-bit descriptor, exact port."""
    dom = 180.0*o/N_OR
    vals = np.zeros(512)
    for dy in range(REGIONS):
        for dx in range(REGIONS):
            r0 = 2*dy + (1 if dy > 1 else 0)
            c0 = 2*dx + (1 if dx > 1 else 0)
            r1 = min(r0+5, N_OR); c1 = min(c0+5, N_OR)
            for yy in range(r0, r1):
                for xx in range(c0, c1):
                    iy = y - DESC_HALF + yy; ix = x - DESC_HALF + xx
                    ang = math.atan2(gy[iy, ix], gx[iy, ix]) * 180.0/math.pi
                    ang = math.fmod(ang, 360.0)
                    if ang < 0: ang += 360.0
                    rel = (ang - dom) / BINW
                    b = int(np.trunc(rel)) % BINS
                    mag = math.sqrt(gx[iy,ix]**2 + gy[iy,ix]**2)
                    vals[dy*128 + dx*32 + b] += W_ROW[yy]*W_COL[xx]/425.0 * mag
    return vals > vals.mean()

def extract(img, scale=1.0):
    if scale != 1.0:
        img = (resize_bilinear(img, scale) + 0.5).astype(np.uint8)
    flat = flat_field(img)
    enh, oidx = oriented_bank(flat)
    gx, gy = gradients(enh)
    cx, cy, cv = candidates(enh)
    order = np.argsort(-cv, kind='stable')
    feats = []
    n_cand = len(cv)
    for i in order:
        if len(feats) >= MAXF: break
        X, Y, V = cx[i], cy[i], cv[i]
        ok = True
        for (fx_, fy_, *_) in feats:
            ddx = X - fx_; ddy = Y - fy_
            if ddx*ddx + ddy*ddy < NMS_R2: ok = False; break
        if not ok: continue
        feats.append((X, Y, int(oidx[Y, X]), descriptors(Y, X, int(oidx[Y, X]), gx, gy)))
    return dict(img=img, flat=flat, enh=enh, oidx=oidx, gx=gx, gy=gy,
                n_cand=n_cand, feats=feats, scale=scale)

def floordiv8(v):
    return v // 8 if v >= 0 else (v - 7) // 8

def score_sets(probe, gallery):
    f1 = probe['feats']; f2 = gallery['feats']
    n1, n2 = len(f1), len(f2)
    if n1 < MINM or n2 < MINM: return dict(score=0, votes=0)
    D1 = np.array([f[3] for f in f1]); D2 = np.array([f[3] for f in f2])
    H = (D1[:,None,:] != D2[None,:,:]).sum(2)          # n1 x n2 hamming
    best_j = H.argmin(1); best_h = H.min(1)
    # translation cluster mode (C tie-break: first i)
    qx = np.array([floordiv8(f1[i][0] - f2[best_j[i]][0]) for i in range(n1)])
    qy = np.array([floordiv8(f1[i][1] - f2[best_j[i]][1]) for i in range(n1)])
    best_cnt, best_qx, best_qy = 0, 0, 0
    for i in range(n1):
        cnt = int(((qx == qx[i]) & (qy == qy[i])).sum())
        if cnt > best_cnt: best_cnt, best_qx, best_qy = cnt, qx[i], qy[i]
    ccx = best_qx*8.0 + 4.0; ccy = best_qy*8.0 + 4.0
    inl = []
    for i in range(n1):
        dx = (f1[i][0] - f2[best_j[i]][0]) - ccx
        dy = (f1[i][1] - f2[best_j[i]][1]) - ccy
        if best_h[i] <= BUDGET and abs(dx) <= POSTOL and abs(dy) <= POSTOL:
            inl.append(i)
    if len(inl) < MINM: return dict(score=0, votes=0, n_inl=len(inl), best_cnt=best_cnt)
    ang_cnt = np.zeros(2*N_OR-1, int)
    for i in inl:
        d = f1[i][2] - f2[best_j[i]][2]
        ang_cnt[d + N_OR - 1] += 1
    mode_k = int(ang_cnt.argmax()) - (N_OR-1)
    mode_deg = 180.0*mode_k/N_OR
    score, good = 0, 0
    for i in inl:
        a1 = 180.0*f1[i][2]/N_OR; a2 = 180.0*f2[best_j[i]][2]/N_OR
        dm = math.fmod(a1 - a2 - mode_deg, 360.0)
        if dm < 0: dm += 360.0
        dang = min(dm, 360.0-dm)
        if dang <= ANGTOL:
            score += 128 - best_h[i]; good += 1
    if good < MINM: return dict(score=0, votes=0, n_inl=len(inl), best_cnt=best_cnt)
    for i in range(n1):
        if best_h[i] > 256: score -= int(best_h[i]) // 2
    return dict(score=score, votes=good, n_inl=len(inl), best_cnt=best_cnt,
                mode_k=mode_k, best_h=best_h, best_j=best_j, inl=inl)

# ---------------- D1: ridge period ----------------
def ridge_period(img, label):
    f = flat_field(img)
    h, w = f.shape
    wy = np.hanning(h); wx = np.hanning(w)
    g = f * np.outer(wy, wx)
    P = np.abs(np.fft.fftshift(np.fft.fft2(g)))**2
    fy = np.fft.fftshift(np.fft.fftfreq(h)); fx = np.fft.fftshift(np.fft.fftfreq(w))
    FY, FX = np.meshgrid(fy, fx, indexing='ij')
    R = np.hypot(FY, FX)
    nb = 60
    rb = np.linspace(0, 0.72, nb+1)
    prof = np.zeros(nb); cnt = np.zeros(nb)
    idx = np.clip(np.digitize(R.ravel(), rb)-1, 0, nb-1)
    np.add.at(prof, idx, P.ravel()); np.add.at(cnt, idx, 1)
    prof /= np.maximum(cnt, 1)
    lo = np.searchsorted(rb, 0.08); hi = np.searchsorted(rb, 0.45)
    k = lo + prof[lo:hi].argmax()
    fpeak = 0.5*(rb[k]+rb[k+1])
    # locate 2D peak (anisotropy check)
    pm = P.copy(); pm[R < 0.06] = 0; pm[R > 0.45] = 0
    iy, ix = np.unravel_index(pm.argmax(), pm.shape)
    print(f"  {label}: FFT peak f={fpeak:.4f} cyc/px -> period {1/fpeak:.2f} px | "
          f"2D peak at (fx={fx[ix]:+.3f}, fy={fy[iy]:+.3f}) period {1/max(np.hypot(fx[ix],fy[iy]),1e-9):.2f}")
    # autocorr ring cross-check
    A = np.abs(np.fft.ifft2(np.abs(np.fft.fft2(f))**2)).real
    A /= A[0,0]
    ys_c = np.arange(h) - h//2; xs_c = np.arange(w) - w//2
    rr = np.hypot(ys_c[:, None], xs_c[None, :])
    prof2 = np.zeros(12); c2 = np.zeros(12)
    idx2 = np.clip(np.digitize(rr.ravel(), np.arange(1.5, 13.5, 1.0))-1, 0, 11)
    np.add.at(prof2, idx2, A.ravel()); np.add.at(c2, idx2, 1)
    prof2 /= np.maximum(c2, 1)
    lags = np.arange(2.0, 14.0)
    pk = prof2.argmax()
    print(f"     autocorr ring peak at lag {lags[pk]:.0f} px (profile {np.round(prof2,3)})")
    # orientation-aware LOCAL ridge frequency: 1-D FFT along each pixel's ridge normal
    enh, oidx = oriented_bank(f)
    th = np.percentile(enh, 55)
    pers = []
    for yy in range(2, h-2, 2):
        for xx in range(2, w-2, 2):
            if enh[yy, xx] < th: continue
            o = int(oidx[yy, xx])
            rad = math.radians(180.0*o/N_OR)
            s = np.arange(-8, 9, 1.0)
            sig = clamp_sample(f, xx + s*math.sin(rad), yy + s*math.cos(rad))
            sig = sig - sig.mean()
            if sig.std() < 0.05: continue
            F = np.abs(np.fft.rfft(sig*np.hanning(17)))
            fr = np.fft.rfftfreq(17)
            kmask = (fr > 1/13.0) & (fr < 1/2.5)
            k = np.where(kmask, F, 0).argmax()
            if fr[k] > 0: pers.append(1.0/fr[k])
    pers = np.array(pers)
    if len(pers) >= 10:
        med = np.median(pers)
        q1, q3 = np.percentile(pers, [25, 75])
        hist, edges = np.histogram(pers, bins=np.arange(2.5, 9.5, 0.25))
        mode = 0.5*(edges[hist.argmax()] + edges[hist.argmax()+1])
        print(f"     LOCAL ridge period (n={len(pers)}): median={med:.2f} IQR=[{q1:.2f},{q3:.2f}] mode={mode:.2f}")
        return med
    return 1.0/fpeak

# ---------------- D2: design period ----------------
def bank_design_period():
    print("\n=== D2: FIR bank design period (synthetic gratings, flat_field+bank) ===")
    S = 128; marg = 16
    rows = []
    for p in np.arange(2.0, 12.01, 0.25):
        stab_ex, sel_ex, stab_mid = [], [], []
        for o in range(N_OR):
            theta = 180.0*o/N_OR
            for ph in (0.0, 0.7):
                for case in ('exact', 'mid'):
                    if case == 'exact':
                        th = theta
                    else:
                        th = theta + 90.0/N_OR   # half a bank step
                    rad = th*math.pi/180.0
                    cs_, sn_ = math.cos(rad), math.sin(rad)
                    # ridge lines along (cos th, -sin th); normal (sin th, cos th)
                    ys_c, xs_c = np.mgrid[0:S, 0:S].astype(float)
                    img = 128 + 120*np.cos(2*np.pi*(xs_c*math.sin(rad) + ys_c*math.cos(rad))/p + ph)
                    img = img.astype(np.uint8)
                    flat = flat_field(img)
                    enh, oidx, stk = oriented_bank(flat, stack=True)
                    sl = oidx[marg:-marg, marg:-marg]
                    best = np.abs(stk[:, marg:-marg, marg:-marg]).max(0)
                    if case == 'exact':
                        stab_ex.append((sl == o).mean())
                        srt = np.sort(np.abs(stk[:, marg:-marg, marg:-marg]), 0)
                        sel_ex.append((srt[-2]/np.maximum(srt[-1],1e-9)).mean())
                    else:
                        ok = (sl == o) | (sl == (o+1) % N_OR)
                        stab_mid.append(ok.mean())
        rows.append((p, np.mean(stab_ex), np.mean(sel_ex), np.mean(stab_mid)))
        print(f"  p={p:5.2f}  stability(exact)={np.mean(stab_ex):.3f}  "
              f"2nd/best={np.mean(sel_ex):.3f}  stability(mid)={np.mean(stab_mid):.3f}")
    rows = np.array(rows)
    band = rows[rows[:,1] > 0.98, 0]
    if len(band): print(f"  winner-reliability>0.98 band: p in [{band.min():.2f}, {band.max():.2f}]")
    sel_ok = rows[rows[:,2] < 0.55, 0]
    if len(sel_ok): print(f"  selectivity 2nd/best<0.55 band: p in [{sel_ok.min():.2f}, {sel_ok.max():.2f}]")

    print("\n=== D2b: END-TO-END synthetic score vs ridge period (full pipeline) ===")
    print("  (same-finger emulation: grating pair, same period, shifted + phase-offset)")
    S = 100
    best_p, best_s = 0, -1
    for p in np.arange(2.5, 8.01, 0.25):
        scs = []
        for (oA, oB, dx, dy, dph) in ((3,3,2,3,0.5),(6,6,-3,2,0.3),(3,4,3,-2,0.7)):
            rad = math.radians(180.0*oA/11)
            ys_c, xs_c = np.mgrid[0:S, 0:S].astype(float)
            A = (128 + 120*np.cos(2*np.pi*(xs_c*math.sin(rad)+ys_c*math.cos(rad))/p)).astype(np.uint8)
            radB = math.radians(180.0*oB/11)
            imgB = 128 + 120*np.cos(2*np.pi*(xs_c*math.sin(radB)+ys_c*math.cos(radB))/p + dph*math.pi)
            Bp = np.roll(np.roll(imgB, dy, 0), dx, 1).astype(np.uint8)
            fa = extract(A, 1.0); fb = extract(Bp, 1.0)
            r = score_sets(fb, fa)
            scs.append(max(r['score'], 0))
        m = float(np.mean(scs))
        star = ' <-- peak' if m > best_s else ''
        if m > best_s: best_s, best_p = m, p
        print(f"  p={p:5.2f}  mean synthetic genuine score={m:7.0f}{star}")
    print(f"  --> SYSTEM-OPTIMAL RIDGE PERIOD (synthetic end-to-end) = {best_p:.2f} px")
    return best_p, rows

# ---------------- main ----------------
base = '/var/home/matt/eh57e-project/captures/'
imgs = {n: load_pgm(base+n+'.pgm') for n in ('first-000','first-001','first-002')}
ctrl = {n: load_pgm(base+f'ctrl1-{i:03d}.pgm') for i, n in enumerate([f'ctrl1-{i:03d}' for i in range(10)])}

print("=== D1: capture ridge periods ===")
periods = {}
for n, im in imgs.items():
    periods[n] = ridge_period(im, n)
pc = np.mean(list(periods.values()))
print(f"  mean capture ridge period = {pc:.3f} px")

design_p, _ = bank_design_period()
print(f"\n  predicted optimal scale = design/capture = {design_p/pc:.3f}")

print("\n=== D3: fine scale sweep, genuine pair first-001 vs first-002 ===")
scales = [round(0.80+0.05*i, 2) for i in range(21)]
cache = {}
def get(name, s):
    key = (name, s)
    if key not in cache:
        src = imgs[name] if name in imgs else ctrl[name]
        cache[key] = extract(src, s)
    return cache[key]
best = None
for s in scales:
    g = get('first-001', s); p = get('first-002', s)
    r = score_sets(p, g)
    line = (f"  s={s:4.2f} feats={len(g['feats']):3d}/{len(p['feats']):3d} "
            f"cand={g['n_cand']:4d}/{p['n_cand']:4d} votes={r['votes']:3d} score={r['score']:5d}")
    print(line)
    if best is None or r['score'] > best[1]: best = (s, r['score'])
print(f"  best scale in sweep: {best[0]} (score {best[1]})")

print("\n=== D4: genuine vs control matrix at best scale and neighbours ===")
for s in [1.15, 1.2, 1.25, 1.3, 1.35]:
    if s not in scales: continue
    print(f"  --- scale {s} ---")
    for a in ('first-000','first-001','first-002'):
        for b in ('first-000','first-001','first-002'):
            if a >= b: continue
            r = score_sets(get(a, s), get(b, s))
            print(f"    genuine {a} vs {b}: votes={r['votes']:3d} score={r['score']:5d}")
    c0, c1, c4 = get('ctrl1-000', s), get('ctrl1-001', s), get('ctrl1-004', s)
    for a in ('first-001','first-002'):
        for cn in ('ctrl1-000','ctrl1-001','ctrl1-004'):
            r = score_sets(get(a, s), get(cn, s))
            print(f"    {a} vs {cn}: votes={r['votes']:3d} score={r['score']:5d}")
    r = score_sets(c0, c1)
    print(f"    ctrl1-000 vs ctrl1-001: votes={r['votes']:3d} score={r['score']:5d}")
    r = score_sets(c0, c4)
    print(f"    ctrl1-000 vs ctrl1-004: votes={r['votes']:3d} score={r['score']:5d}")

print("\n=== D5: descriptor Hamming distributions at scale", best[0], "===")
s = best[0]
f1 = get('first-001', s); f2 = get('first-002', s)
D1 = np.array([f[3] for f in f1['feats']]); D2 = np.array([f[3] for f in f2['feats']])
Hall = (D1[:,None,:] != D2[None,:,:]).sum(2)
print(f"  all cross-press NN hamming: min={Hall.min(1).min()} "
      f"mean={Hall.min(1).mean():.1f} | all-pairs mean={Hall.mean():.1f} (random 512-bit = 256)")
r = score_sets(f1, f2)
if 'best_h' in r:
    inh = r['best_h'][r['inl']]
    print(f"  inlier NN hamming (n={len(inh)}): {sorted(inh)}  budget={BUDGET}")
    print(f"  non-inlier NN hamming mean={np.delete(r['best_h'], r['inl']).mean():.1f}")

# repeatability at best scale
def repeatability(A, B, tol=4.0):
    fa = A['feats']; fb = B['feats']
    pa = np.array([[f[0], f[1]] for f in fa], float)
    pb = np.array([[f[0], f[1]] for f in fb], float)
    if len(pa) == 0 or len(pb) == 0: return 0, 0
    # best translation via brute mode on rounded offsets
    offs = []
    for i in range(len(pa)):
        for j in range(len(pb)):
            offs.append(pa[i] - pb[j])
    offs = np.array(offs)
    key = np.round(offs/4).astype(int)
    uniq, cnt = np.unique(key, axis=0, return_counts=True)
    t = offs[np.argmax(cnt)]
    d = np.abs((pa[:,None,:] - (pb[None,:,:] + t))).max(2)
    m = (d <= tol).any(1).sum()
    rep = m / max(len(pa), len(pb))
    return m, rep
m, rep = repeatability(f1, f2)
print(f"  IP correspondence (<=4px after best translation): {m} -> repeatability {rep:.2f}")
oa = np.array([f[2] for f in f1['feats']]); ob = np.array([f[2] for f in f2['feats']])
print(f"  orientation histogram press A: {np.bincount(oa, minlength=11)}")
print(f"  orientation histogram press B: {np.bincount(ob, minlength=11)}")
print("DONE")