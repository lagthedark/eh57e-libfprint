#!/usr/bin/env python3
"""Round 3: C-exact port + orientation-stability-vs-scale (the key metric)."""
import numpy as np, math, subprocess

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
    ys = (np.arange(H) + 0.5) / s - 0.5; xs = (np.arange(W) + 0.5) / s - 0.5
    ys = np.clip(ys, 0, h-1); xs = np.clip(xs, 0, w-1)
    yi = np.clip(np.floor(ys).astype(int), 0, h-2); xi = np.clip(np.floor(xs).astype(int), 0, w-2)
    fy = (ys - yi)[:, None]; fx = (xs - xi)[None, :]
    A = img[np.ix_(yi, xi)]; B = img[np.ix_(yi, xi+1)]
    C = img[np.ix_(yi+1, xi)]; D = img[np.ix_(yi+1, xi+1)]
    return A*(1-fy)*(1-fx) + B*(1-fy)*fx + C*fy*(1-fx) + D*fy*fx

src = open('/tmp/opencode/eh57e_diag.py').read()
hdr_end = src.index("# ---------------- PGM")
exec(src[:hdr_end])          # constants
start = src.index("# ---------------- pipeline port")
end = src.index("# ---------------- D1: ridge period")
exec(src[start:end])

# ---- C-exact selection sort (UNSTABLE, replicating egis057e-matcher.c) ----
def c_sort_order(vals):
    idx = list(range(len(vals)))
    for i in range(len(idx)):
        for j in range(i+1, len(idx)):
            if vals[idx[j]] > vals[idx[i]]:
                idx[i], idx[j] = idx[j], idx[i]
    return idx

def extract_c(img, scale=1.0):
    if scale != 1.0:
        img = (resize_bilinear(img, scale) + 0.5).astype(np.uint8)
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
            ddx = X - fx_; ddy = Y - fy_
            if ddx*ddx + ddy*ddy < NMS_R2: ok = False; break
        if not ok: continue
        o = int(oidx[Y, X])
        feats.append((X, Y, o, descriptors(Y, X, o, gx, gy)))
    # per-pixel best/second-best response ratio on finger area (enh percentile)
    srt = np.sort(np.abs(stk), 0)
    ratio = np.where(srt[-1] > 1e-12, srt[-2]/np.maximum(srt[-1], 1e-12), 1.0)
    strong = enh > np.percentile(enh, 75)
    return dict(img=img, enh=enh, oidx=oidx, feats=feats, scale=scale,
                n_cand=len(cv), resp2nd=np.mean(ratio[strong]))

base = '/var/home/matt/eh57e-project/captures/'
names = ['first-001','first-002','ctrl1-000','ctrl1-001','ctrl1-002','ctrl1-003',
         'ctrl1-005','ctrl1-009']
imgs = {n: load_pgm(base+n+'.pgm') for n in names}

print("=== A: C-exact port validation ===")
binp = '/var/home/matt/eh57e-project/libfprint-build/tools/egis057e-match-test'
for s in (1.0, 1.25):
    ga = extract_c(imgs['first-001'], s); gb = extract_c(imgs['first-002'], s)
    r = score_sets(gb, ga)
    def save_pgm(path, img):
        h, w = img.shape
        with open(path, 'wb') as f:
            f.write(f"P5\n{w} {h}\n255\n".encode()); f.write(img.astype(np.uint8).tobytes())
    save_pgm('/tmp/opencode/v_g.pgm', ga['img']); save_pgm('/tmp/opencode/v_p.pgm', gb['img'])
    out = subprocess.run([binp, 'score', '/tmp/opencode/v_g.pgm', '/tmp/opencode/v_p.pgm'],
                         capture_output=True, text=True)
    print(f"  s={s}: C harness: {out.stdout.strip()}  | my port: feats={len(ga['feats'])}/"
          f"{len(gb['feats'])} votes={r['votes']} score={r['score']}  "
          f"{'MATCH' if f'votes={r[chr(39)+chr(39) if False else 'votes']}' in out.stdout else ''}")

print("\n=== B: orientation-stability vs scale (real pairs) ===")
def correspondence_stats(ga, gb):
    fa, fb = ga['feats'], gb['feats']
    s = ga['scale']
    pa = np.array([[f[0]/s, f[1]/s] for f in fa]); pb = np.array([[f[0]/s, f[1]/s] for f in fb])
    if len(pa) < 5 or len(pb) < 5: return None
    offs = (pa[:, None, :] - pb[None, :, :]).reshape(-1, 2)
    key = np.round(offs/4).astype(int)
    uniq, cnt = np.unique(key, axis=0, return_counts=True)
    t = offs[np.argmax(cnt)]
    d = np.abs(pa[:, None, :] - (pb[None, :, :] + t)).max(2)
    ii, jj = np.nonzero(d <= 3.0)
    # dedupe: first j per i
    firstj = {}
    for i, j in zip(ii, jj):
        if i not in firstj: firstj[i] = j
    n = len(firstj)
    if n == 0: return dict(n_corr=0)
    ex = sum(1 for i, j in firstj.items() if fa[i][2] == fb[j][2])
    pm = sum(1 for i, j in firstj.items() if abs(fa[i][2] - fb[j][2]) <= 1)
    # hamming of corresponding pairs
    Hc = [int((fa[i][3] != fb[j][3]).sum()) for i, j in firstj.items()]
    return dict(n_corr=n, rep=n/max(len(pa), len(pb)), ex=ex, pm=pm,
                hmean=float(np.mean(Hc)), hmin=int(np.min(Hc)))

cache = {}
def get(name, s):
    key = (name, s)
    if key not in cache: cache[key] = extract_c(imgs[name], s)
    return cache[key]

pairs = [('first-001','first-002'), ('first-002','ctrl1-001'), ('first-001','ctrl1-000')]
scales = [0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.8, 2.0]
for a, b in pairs:
    print(f"  pair {a} x {b}:")
    print("   s     corr  rep   orient=  or<=1  hamM  resp2nd | votes score")
    for s in scales:
        ga, gb = get(a, s), get(b, s)
        cs = correspondence_stats(ga, gb)
        r = score_sets(gb, ga)
        if cs and cs['n_corr']:
            print(f"  {s:4.2f} {cs['n_corr']:5d} {cs['rep']:.2f} {cs['ex']:5d} {cs['pm']:5d} "
                  f"{cs['hmean']:6.0f} {ga['resp2nd']:.3f} | {r['votes']:4d} {r['score']:6d}")
        else:
            print(f"  {s:4.2f}    0  ---- no correspondences ---- resp2nd={ga['resp2nd']:.3f} "
                  f"| {r['votes']:4d} {r['score']:6d}")

print("\n=== C: native-scale forensics, pair first-001 x first-002 ===")
ga, gb = get('first-001', 1.0), get('first-002', 1.0)
fa, fb = ga['feats'], gb['feats']
D1 = np.array([f[3] for f in fa]); D2 = np.array([f[3] for f in fb])
H = (D1[:, None, :] != D2[None, :, :]).sum(2)
print(f"  feats {len(fa)}/{len(fb)}; all-pairs hamming mean={H.mean():.0f} min={H.min()}")
cs = correspondence_stats(ga, gb)
print(f"  correspondences: {cs}")
r = score_sets(gb, ga)
print(f"  votes={r['votes']} score={r['score']} best_cnt={r.get('best_cnt',0)}")
# orientation histogram of both
oa = np.bincount([f[2] for f in fa], minlength=11); ob = np.bincount([f[2] for f in fb], minlength=11)
print(f"  orient hist A: {oa}")
print(f"  orient hist B: {ob}")

print("\n=== D: quality spread at s=1.0 and s=1.45, all genuine pairs ===")
for s in (1.0, 1.45):
    print(f"  s={s}:")
    ns = ['first-001','first-002','ctrl1-000','ctrl1-001','ctrl1-002','ctrl1-003','ctrl1-005','ctrl1-009']
    for i in range(len(ns)):
        for j in range(i+1, len(ns)):
            r = score_sets(get(ns[j], s), get(ns[i], s))
            if r['score'] > 0 or r['votes'] > 0:
                print(f"    {ns[i]} x {ns[j]}: votes={r['votes']:3d} score={r['score']:5d}")
print("DONE3")