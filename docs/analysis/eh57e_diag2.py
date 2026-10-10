#!/usr/bin/env python3
"""EH57E diagnostics round 2: forensics at optima, FPN check, impostor baseline."""
import numpy as np, math, subprocess, sys
exec(open('/tmp/opencode/eh57e_diag.py').read().split("# ---------------- PGM")[0])  # constants + funcs
exec(open('/tmp/opencode/eh57e_diag.py').read().split("# ---------------- pipeline port")[1].split("def load_pgm")[0]) if False else None

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

# re-import pipeline funcs from the diag script text
src = open('/tmp/opencode/eh57e_diag.py').read()
start = src.index("# ---------------- pipeline port")
end = src.index("# ---------------- D1: ridge period")
exec(src[start:end])

base = '/var/home/matt/eh57e-project/captures/'
names = ['first-000','first-001','first-002'] + [f'ctrl1-{i:03d}' for i in range(10)]
imgs = {n: load_pgm(base+n+'.pgm') for n in names}

print("=== raw stats ===")
for n in names:
    im = imgs[n]
    print(f"  {n}: mean={im.mean():6.1f} std={im.std():5.1f} p5={np.percentile(im,5):5.1f} "
          f"p95={np.percentile(im,95):6.1f}")

print("\n=== FPN check: border (non-finger) region spectrum ===")
for n in ('first-000','first-001','first-002','ctrl1-000'):
    im = imgs[n].astype(float)
    border = np.concatenate([im[:6].ravel(), im[-6:].ravel(), im[:, :6].ravel(), im[:, -6:].ravel()])
    border = border - border.mean()
    F = np.abs(np.fft.rfft(border * np.hanning(len(border))))
    fr = np.fft.rfftfreq(len(border))
    k = F[1:].argmax()+1
    print(f"  {n}: border dominant f={fr[k]:.3f} cyc/px (period {1/fr[k]:.1f} px), "
          f"border std={border.std():.1f}, frame std={im.std():.1f}")

print("\n=== A: port validation vs C harness on identical inputs ===")
def save_pgm(path, img):
    h, w = img.shape
    with open(path, 'wb') as f:
        f.write(f"P5\n{w} {h}\n255\n".encode())
        f.write(img.astype(np.uint8).tobytes())
r125 = resize_bilinear(imgs['first-001'], 1.25).astype(np.uint8)
r125b = resize_bilinear(imgs['first-002'], 1.25).astype(np.uint8)
save_pgm('/tmp/opencode/f1_s125.pgm', r125)
save_pgm('/tmp/opencode/f2_s125.pgm', r125b)
binp = '/var/home/matt/eh57e-project/libfprint-build/tools/egis057e-match-test'
out = subprocess.run([binp, 'score', '/tmp/opencode/f1_s125.pgm', '/tmp/opencode/f2_s125.pgm'],
                     capture_output=True, text=True)
print("  C harness @1.25 (my resize):", out.stdout.strip() or out.stderr.strip())
g = extract(imgs['first-001'], 1.25); p = extract(imgs['first-002'], 1.25)
r = score_sets(p, g)
print(f"  my port     @1.25 (my resize): feats={len(g['feats'])}/{len(p['feats'])} "
      f"votes={r['votes']} score={r['score']}")
save_pgm('/tmp/opencode/f1_nat.pgm', imgs['first-001'])
save_pgm('/tmp/opencode/f2_nat.pgm', imgs['first-002'])
out = subprocess.run([binp, 'score', '/tmp/opencode/f1_nat.pgm', '/tmp/opencode/f2_nat.pgm'],
                     capture_output=True, text=True)
print("  C harness @native:", out.stdout.strip() or out.stderr.strip())

print("\n=== B: scale sweeps for all 3 genuine pairs ===")
cache = {}
def get(name, s):
    key = (name, s)
    if key not in cache: cache[key] = extract(imgs[name], s)
    return cache[key]
scales = [round(0.80+0.05*i, 2) for i in range(21)]
for a, b in (('first-000','first-001'), ('first-000','first-002'), ('first-001','first-002')):
    print(f"  pair {a} vs {b}:")
    row = []
    for s in scales:
        r = score_sets(get(b, s), get(a, s))
        row.append((s, r['votes'], r['score']))
    nz = [x for x in row if x[2] > 0]
    print("   " + "  ".join(f"s{t[0]:.2f}:{t[2]}" for t in row))
    print(f"   non-zero: {nz}")

print("\n=== C: forensics at per-pair best scales ===")
def forensic(a, b, s, label):
    ga, gb = get(a, s), get(b, s)
    r = score_sets(gb, ga)
    fa, fb = ga['feats'], gb['feats']
    D1 = np.array([f[3] for f in fa]); D2 = np.array([f[3] for f in fb])
    H = (D1[:, None, :] != D2[None, :, :]).sum(2)
    nnH = H.min(1)
    # physical IP correspondence (native coords)
    pa = np.array([[f[0]/s, f[1]/s] for f in fa]); pb = np.array([[f[0]/s, f[1]/s] for f in fb])
    offs = (pa[:, None, :] - pb[None, :, :]).reshape(-1, 2)
    key = np.round(offs/4).astype(int)
    uniq, cnt = np.unique(key, axis=0, return_counts=True)
    t = offs[np.argmax(cnt)]
    d = np.abs(pa[:, None, :] - (pb[None, :, :] + t)).max(2)
    corr = int((d <= 3).any(1).sum())
    rep = corr/max(len(pa), len(pb))
    # orientation agreement among corresponding pairs
    ag = 0; tot = 0
    for i in range(len(fa)):
        js = np.where(d[i] <= 3)[0]
        for j in js[:1]:
            tot += 1
            if fa[i][2] == fb[j][2]: ag += 1
    print(f"  [{label}] s={s}: feats={len(fa)}/{len(fb)} votes={r['votes']} score={r['score']} "
          f"best_cnt={r.get('best_cnt',0)} n_inl={r.get('n_inl',0)}")
    print(f"     IP corr(<=3px native): {corr} repeatability={rep:.2f} | orient agreement {ag}/{tot}")
    print(f"     NN hamming: min={nnH.min()} mean={nnH.mean():.0f} | <=115: {(nnH<=115).sum()}/{len(nnH)} "
          f"| <=150: {(nnH<=150).sum()} | all-pairs mean={H.mean():.0f}")
    if 'inl' in r and len(r['inl']):
        print(f"     inlier hamming: {sorted(r['best_h'][r['inl']].tolist())}")

forensic('first-001','first-002', 1.45, 'pair 001x002')
forensic('first-001','first-002', 1.80, 'pair 001x002')
forensic('first-000','first-001', 1.10, 'pair 000x001')
forensic('first-000','first-002', 1.10, 'pair 000x002')

print("\n=== D: impostor / control analysis at s=1.45 ===")
cs = [f'ctrl1-{i:03d}' for i in range(10)]
# ctrl1 internal pair scores
cnz = []
for i in range(0, 9, 3):
    r = score_sets(get(cs[i+1], 1.45), get(cs[i], 1.45))
    cnz.append((cs[i], cs[i+1], r['votes'], r['score']))
    print(f"  ctrl internal {cs[i]} vs {cs[i+1]}: votes={r['votes']} score={r['score']}")
print("  first vs ctrl (s=1.45):")
for a in ('first-001','first-002'):
    for c in cs[:5]:
        r = score_sets(get(c, 1.45), get(a, 1.45))
        print(f"    {a} vs {c}: votes={r['votes']:3d} score={r['score']:5d}")

print("\n=== E: parameter sensitivity at s=1.45, pair 001x002 ===")
import importlib
ga, gb = get('first-001', 1.45), get('first-002', 1.45)
def resc(g1, g2, budget, postol, minm, nms=None):
    global BUDGET, POSTOL, MINM
    b0, p0, m0 = BUDGET, POSTOL, MINM
    BUDGET, POSTOL, MINM = budget, postol, minm
    r = score_sets(g2, g1)
    BUDGET, POSTOL, MINM = b0, p0, m0
    return r
for budget in (115, 130, 150, 170):
    r = resc(ga, gb, budget, 8, 5)
    print(f"  budget={budget}: votes={r['votes']} score={r['score']} n_inl={r.get('n_inl',0)}")
for postol in (8, 10, 12):
    r = resc(ga, gb, 115, postol, 5)
    print(f"  postol={postol}: votes={r['votes']} score={r['score']} n_inl={r.get('n_inl',0)}")
for minm in (3, 4, 5):
    r = resc(ga, gb, 115, 8, minm)
    print(f"  minm={minm}: votes={r['votes']} score={r['score']} n_inl={r.get('n_inl',0)}")
print("DONE2")