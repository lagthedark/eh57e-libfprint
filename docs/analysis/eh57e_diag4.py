#!/usr/bin/env python3
"""Round 4: vendor-faithful front-end experiments (bg-subtract, CLAHE-ish)
vs orientation stability. Plus period spread across all same-finger frames."""
import numpy as np, math

def load_pgm(path):
    with open(path, 'rb') as f: data = f.read()
    toks, i = [], 0
    while len(toks) < 4:
        while i < len(data) and data[i:i+1].isspace(): i += 1
        if data[i:i+1] == b'#':
            while i < len(data) and data[i:i+1] != b'\n': i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace(): j += 1
        toks.append(data[i:j]); i = j
    w, h, mx = int(toks[1]), int(toks[2]), int(toks[3])
    return np.frombuffer(data, np.uint8, w*h, i+1).reshape(h, w).copy()

def resize_bilinear(img, s):
    h, w = img.shape
    H, W = int(round(h*s)), int(round(w*s))
    ys = (np.arange(H) + 0.5)/s - 0.5; xs = (np.arange(W) + 0.5)/s - 0.5
    ys = np.clip(ys, 0, h-1); xs = np.clip(xs, 0, w-1)
    yi = np.clip(np.floor(ys).astype(int), 0, h-2); xi = np.clip(np.floor(xs).astype(int), 0, w-2)
    fy = (ys-yi)[:, None]; fx = (xs-xi)[None, :]
    A = img[np.ix_(yi,xi)]; B = img[np.ix_(yi,xi+1)]; C = img[np.ix_(yi+1,xi)]; D = img[np.ix_(yi+1,xi+1)]
    return A*(1-fy)*(1-fx)+B*(1-fy)*fx+C*fy*(1-fx)+D*fy*fx

src = open('/tmp/opencode/eh57e_diag.py').read()
exec(src[:src.index("# ---------------- PGM")])
p0 = src.index("# ---------------- pipeline port"); p1 = src.index("# ---------------- D1: ridge period")
exec(src[p0:p1])

def c_sort_order(vals):
    idx = list(range(len(vals)))
    for i in range(len(idx)):
        for j in range(i+1, len(idx)):
            if vals[idx[j]] > vals[idx[i]]: idx[i], idx[j] = idx[j], idx[i]
    return idx

def extract_c(img, scale=1.0):
    if scale != 1.0: img = (resize_bilinear(img, scale)+0.5).astype(np.uint8)
    flat = flat_field(img)
    enh, oidx, stk = oriented_bank(flat, stack=True)
    gx, gy = gradients(enh)
    cx, cy, cv = candidates(enh)
    order = c_sort_order(cv.tolist())
    feats = []
    for i in order:
        if len(feats) >= MAXF: break
        X, Y = int(cx[i]), int(cy[i])
        ok = True
        for (fx_, fy_, *_) in feats:
            if (X-fx_)**2 + (Y-fy_)**2 < NMS_R2: ok = False; break
        if not ok: continue
        o = int(oidx[Y, X])
        feats.append((X, Y, o, descriptors(Y, X, o, gx, gy)))
    srt = np.sort(np.abs(stk), 0)
    ratio = np.where(srt[-1] > 1e-12, srt[-2]/np.maximum(srt[-1], 1e-12), 1.0)
    strong = enh > np.percentile(enh, 75)
    return dict(img=img, enh=enh, oidx=oidx, feats=feats, scale=scale,
                n_cand=len(cv), resp2nd=float(np.mean(ratio[strong])))

def corr_stats(ga, gb):
    fa, fb = ga['feats'], gb['feats']; s = ga['scale']
    pa = np.array([[f[0]/s, f[1]/s] for f in fa]); pb = np.array([[f[0]/s, f[1]/s] for f in fb])
    if len(pa) < 5 or len(pb) < 5: return None
    offs = (pa[:, None, :] - pb[None, :, :]).reshape(-1, 2)
    key = np.round(offs/4).astype(int)
    uniq, cnt = np.unique(key, axis=0, return_counts=True)
    t = offs[np.argmax(cnt)]
    d = np.abs(pa[:, None, :] - (pb[None, :, :] + t)).max(2)
    ii, jj = np.nonzero(d <= 3.0)
    firstj = {}
    for i, j in zip(ii, jj):
        if i not in firstj: firstj[i] = j
    if not firstj: return None
    ex = sum(1 for i, j in firstj.items() if fa[i][2] == fb[j][2])
    pm = sum(1 for i, j in firstj.items() if abs(fa[i][2]-fb[j][2]) <= 1)
    Hc = [int((fa[i][3] != fb[j][3]).sum()) for i, j in firstj.items()]
    return dict(n=len(firstj), ex=ex, pm=pm, hmean=float(np.mean(Hc)), hmin=min(Hc) if Hc else 512)

base = '/var/home/matt/eh57e-project/captures/'
bg = load_pgm(base+'first-000.pgm').astype(np.int16)   # no-press frame (std 3.1)

def bg_subtract(img, bg):
    v = img.astype(np.int16) - bg.astype(np.int16)
    return np.clip(v, 0, 255).astype(np.uint8)

frames = ['first-001','first-002','ctrl1-000','ctrl1-001','ctrl1-002','ctrl1-005','ctrl1-009']
raw = {n: load_pgm(base+n+'.pgm') for n in frames}

print("=== period spread (local, orientation-aware) across same-finger frames ===")
for n in frames:
    im = raw[n]
    f = flat_field(im)
    enh, oidx = oriented_bank(f)
    h, w = f.shape
    th = np.percentile(enh, 55)
    pers = []
    for yy in range(2, h-2, 2):
        for xx in range(2, w-2, 2):
            if enh[yy, xx] < th: continue
            o = int(oidx[yy, xx]); rad = math.radians(180.0*o/11)
            s = np.arange(-8, 9, 1.0)
            sig = clamp_sample(f, xx + s*math.sin(rad), yy + s*math.cos(rad))
            sig = sig - sig.mean()
            if sig.std() < 0.05: continue
            F = np.abs(np.fft.rfft(sig*np.hanning(17))); fr = np.fft.rfftfreq(17)
            kmask = (fr > 1/13.0) & (fr < 1/2.5)
            k = np.where(kmask, F, 0).argmax()
            if fr[k] > 0: pers.append(1.0/fr[k])
    pers = np.array(pers)
    if len(pers):
        hist, edges = np.histogram(pers, bins=np.arange(2.5, 9.5, 0.25))
        mode = 0.5*(edges[hist.argmax()]+edges[hist.argmax()+1])
        print(f"  {n}: median={np.median(pers):.2f} mode={mode:.2f} "
              f"frac in [4.5,6.5]: {((pers>4.5)&(pers<6.5)).mean():.2f} (n={len(pers)})")

print("\n=== front-end variants: orientation stability + genuine score (native scale) ===")
variants = {
    'raw': lambda im: im,
    'bg-sub(first-000)': lambda im: bg_subtract(im, bg),
}
pairs = [('first-001','first-002'), ('first-001','ctrl1-000'), ('first-002','ctrl1-001')]
for vname, fn in variants.items():
    print(f"  --- variant: {vname} ---")
    ex = {}
    for n in frames:
        ex[n] = extract_c(fn(raw[n]), 1.0)
    for a, b in pairs:
        ga, gb = ex[a], ex[b]
        cs = corr_stats(ga, gb)
        r = score_sets(gb, ga)
        if cs:
            print(f"    {a} x {b}: feats={len(ga['feats'])}/{len(gb['feats'])} "
                  f"corr={cs['n']:3d} exact={cs['ex']:3d} pm1={cs['pm']:3d} "
                  f"hamM={cs['hmean']:5.0f} hamMin={cs['hmin']:3d} resp2nd={ga['resp2nd']:.3f} "
                  f"| votes={r['votes']} score={r['score']}")
        else:
            print(f"    {a} x {b}: no corr | votes={r['votes']} score={r['score']}")
print("DONE4")