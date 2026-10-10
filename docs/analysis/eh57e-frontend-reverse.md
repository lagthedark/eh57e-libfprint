# EH57E vendor engine — front-end reverse engineering

Target: `EgisTouchFPEngine057E.dll` (PE32+ x86-64, image base `0x180000000`).
Method: Ghidra 12.0.4 headless auto-analysis + decompilation of every function that
references the known tables, plus raw memory dumps. Project: `/tmp/opencode/ghidra_proj/Eh57e`.

> **Headline result (answers Q3):** the vendor does **NOT** compute a per-pixel argmax
> over the 11 kernels at `0xd6ed0`. In the EH57E engine that table is referenced from
> exactly one site (`FUN_180016ff0`, mode 2) which selects **one** kernel by a config
> scalar and applies it as a **separable 2-D smoothing** (same 1-D profile on both axes).
> Ridge orientation is obtained from a **structure tensor** (Sobel/derivative family)
> and converted to degrees with the **atan2 LUT at `0xd7030`**; the per-feature dominant
> orientation is a **response-weighted orientation histogram** over an 11×11 pyramid-weighted
> window (`FUN_180017f80`). This is why a plain "argmax over the FIR bank" port has a tiny
> separation margin.

---

## 0. Corrected table map (the ones that matter)

| table | RVA | actual role (verified) |
|---|---|---|
| FIR kernel family | `0xd6ed0` | 11 × 1-D **symmetric low-pass / ridge-scale** profiles, widths `[5,5,5,7,7,7,9,9,9,11,11]`, each sum `32768`. Referenced **only** by `FUN_180016ff0`. |
| kernel lengths | `0x59bc8` | bytes `[5,5,5,7,7,7,9,9,9,11,11]` (length of each kernel) |
| cumulative tap offsets | built on stack in `FUN_180016ff0` | bytes `[0,5,10,15,22,29,36,45,54,63,74]` = i32 start offset of each kernel in `0xd6ed0` |
| descriptor window weights | `0xd7140` | 11×11 = 121 i32 weights, peak 425 (stride `0x2c`=44 in code) |
| **atan2 LUT** | **`0xd7030`** | 128 × u16, strictly monotonic 0…358. Used by `FUN_180017eb0` with a `720−x` / quadrant correction. |
| cos Q16 | `0xca0a0` | 360 × i32, `cos(i°)*65536` (starts 65536, 65526, 65496, …) |
| sin Q16 | `0xd19a0` | 360 × i32, `sin(i°)*65536` (starts 0, 1144, 2287, …) |
| aux coeff/table | `0xd1f40` | **not cos** (sin ends exactly here). Used by `FUN_1800194e0`/`FUN_180019c00`/`FUN_1800199a0` (border/pad filtering). |
| 2× 5×5 smoothing kernels | `0xd7330` | two 5×5 i32 kernels (mode-1 smoothing) |
| mode-1 byte table | `0x59bbe` | aux byte table used only by `FUN_180016ff0` mode 1 |
| ~~atan2 `0xe8006`~~ | `0xe8006` | **false positive**: `0xe8006` lies in the `.reloc` section. |
| ~~sensor cfg `0x40a34`~~ | `0x40a34` | **false positive**: no code xref, no data pointer. Real config is the heap struct at `DAT_1800e2830`. |

`objdump` reference scan (`getReferenceIterator` over `.text`):

```
func 180016ff0 -> 180059bbe 180059bc8 180059bd2 1800d6ed0 1800d6ff8 1800d7330   # FIR dispatch
func 180017eb0 -> 1800d7030                                                     # atan2
func 180017f80 -> 1800d7140                                                     # pyramid
func 180018960 -> 1800ca0a0 1800d19a0                                           # sin/cos
func 180009650/180009810/180009ab0/18000a1b0/18000cb80 -> 1800ca0a0 1800d19a0  # align/rotate
```

---

## 1. Function map (assigned names)

| VA | assigned name | role |
|---|---|---|
| `0x180018f30` | `engine_process_image` | top-level front-end driver |
| `0x180016ff0` | `fir_dispatch` | mode 1/2/3 filter entry |
| `0x180016be0` | `fir_scale_filter` | separable 2-D smoothing with one 1-D kernel |
| `0x180016a30` | `tap_index_map` | identity+clamp index map (no rotation) |
| `0x1800168b0` | `conv2d` | generic 2-D convolution (arbitrary 2-D kernel) |
| `0x180017390` | `structure_tensor` | computes Ix, Iy, Ixx, Ixy, Iyy (mode 0 default) |
| `0x180017160` | `directional_derivative` | separable [smooth]×[−1,0,+1] derivative |
| `0x180017710` | `hessian_det_response` | det = Ixx·Iyy − Ixy², + polarity byte |
| `0x1800178a0` | `detect_interest_points` | strict 3×3 local maxima + density NMS |
| `0x180017be0` | `refine_subpixel` | quadratic (Hessian) sub-pixel fit |
| `0x180018af0` | `build_feature_descriptors` | per-feature orientation + binary descriptor |
| `0x180017f80` | `orientation_histogram` | weighted grad histogram over 11×11 window |
| `0x180018960` | `pack_binary_descriptor` | pairwise-comparison bit packing |
| `0x180018380` | `block_means_grid` | rotated n×n block means (bilinear) |
| `0x180017eb0` | `atan2_deg_lut` | `atan2` → degrees via `0xd7030` |
| `0x180014720` | `quality_score` | per-pixel |g| and ridge-coherence quality byte |
| `0x180012540` / `0x1800129b0` | `normalize_mode1/2` | normalisation / foreground mask |
| `0x180011870` | `global_stretch` | global min/max stretch to [0,255] |
| `0x180008150` | `local_contrast` | CLAHE-like local normalisation |
| `0x180011b20` | `bilinear_resample` | fixed-point bilinear resize |
| `0x180008b80` | `border_pad` | reflected border padding |
| `0x180015e80` | `u8_to_u32` | 8-bit → 32-bit image (`×0x202020>>16`) |
| `0x18000ac90` | `compare_descriptors` | NN Hamming + ratio test + score accumulation |
| `0x180009ab0` | `verify_matches_geom` | candidate + orientation/Hamming + reproject check |
| `0x18000a1b0` | `translation_hist_align` | translation/angle histogram alignment |
| `0x18000fe70` / `0x180015320` | `config_defaults` / `config_by_sensor` | populate `DAT_1800e2830` |

---

## 2. Q1 — Input geometry and resampling

`engine_process_image` (`FUN_180018f30`) receives the raw capture `(param_2 = width, param_3 = height)`
and, before the FIR bank, does:

1. **Border padding** (only if `cfg[0x10] > 0`):
   ```c
   iVar4 = cfg[0x10];
   iVar22 = param_3 + (iVar4 % 10) * 2;   // new height
   iVar17 = param_2 + (iVar4 / 10) * 2;   // new width
   border_pad(param_1data, plVar6, &param_2, &param_3, iVar4%10, iVar4/10, ...);
   ```
   `border_pad` (`FUN_180008b80`) mirrors borders out by `cfg[0x10]%10` columns / `cfg[0x10]/10` rows
   (`FUN_1800194e0` + the coeff table `0xd1f40`).
2. **Resample** by `cfg[5]/1024` (Q10):
   ```c
   iVar4  = cfg[5] * param_2;  iVar17 = cfg[5] * param_3;
   uVar16 = (iVar4  + 0x1ff) >> 10;   // out width
   uVar23 = (iVar17 + 0x1ff) >> 10;   // out height
   resampled = bilinear_resample(img, param_2, param_3, uVar16, uVar23);   // FUN_180011b20
   ```
   `bilinear_resample` is fixed-point (10-bit) separable bilinear.

**`cfg[5]` default = `0x700` = 1792 → 1792/1024 = 1.75×.** This is the "1.75× resample" in the
EH575 notes and it holds for the EH57E default config (`FUN_18000fe70`, `FUN_180015320`).

So for a 70×57 EH57E capture the working image is ≈ `round(1.75·70)=122 × round(1.75·57)=100`
(with `cfg[0x10]=0` → no padding). The front-end never assumes a fixed dimension; it is
`round(cfg[5]/1024 · sensor_dim)`. There is **no** hard-coded 70×57 in the code — 70/57 are
sensor dimensions supplied by the WBF capture / calibration, not constants in `.text`.

---

## 3. Q2 — Preprocessing before the bank

Order in `engine_process_image`:

1. **Raw normalisation branch** keyed on `cfg[0xb]`:
   - `cfg[0xb]==1` → `FUN_1800134b0` (local background/offset variant)
   - `cfg[0xb]==2` → alloc, then `new = clamp(input − blurred + 0x80, 0, 255)` — background subtraction
     / local mean removal (this is the EH575-style FPN/background removal).
2. **Resample** (Q1).
3. **Normalisation** keyed on `cfg[6]`:
   - `<1` → memset 0 (bypass)
   - `==1` → `FUN_180012540` (block-based gain/offset normalisation, returns coverage)
   - `==2` → `FUN_1800129b0` (6×6 block mean+variance → foreground mask, thresholds
     `mean−?`/variance, returns coverage). Default `cfg[6]==1`.
4. **Enhancement** keyed on `cfg[7]`:
   - `==1` → `FUN_180011870` **global contrast stretch**:
     `scale = 0x3fc00 / (max−min)`; `out = clamp(((x−min)*scale + 0x200) >> 10, 0, 255)`.
   - `==2` → `FUN_180008150` **local/CLAHE-like** contrast normalisation using running
     column sums and a local low/high pair. Default `cfg[7]==2`.
5. `u8_to_u32` (`FUN_180015e80`): 8-bit → 32-bit via `byte*0x202020>>16` (≈ ×32), needed by the
   integer convolution pipeline.

**There is no gamma/power-law stage.** "FPN removal" is the raw branch (1) + normalisation (3);
"local contrast normalisation" is step 4 (`cfg[7]==2`).

---

## 4. The FIR bank — what it actually is

`engine_process_image` calls `fir_dispatch(plVar10, cfg)` **once**:

```c
mode = cfg[8];                       // FUN_180016ff0: iVar2 = param_2[8]
if (mode == 2) {                     // default (config_by_sensor sets cfg[8]=2)
    idx = min(cfg[0] - 10, 10);      // cfg[0] default 0x10=16 -> idx=6
    // cumulative byte-offset table on stack:
    //   [0,5,10,15,22,29,36,45,54,63,74]
    fir_scale_filter(image, &FIR[off[idx]], len[idx]);   // FUN_180016be0
}
```

`fir_scale_filter` (`FUN_180016be0`) performs **two passes with the same 1-D kernel**:

- pass 1: horizontal 1-D dot product with `tap_index_map(width, len)` (identity + edge clamp),
  result `(sum + 0x4000) >> 15` → temporary;
- pass 2: transpose-like pass with `tap_index_map(height, len)` → vertical 1-D dot product,
  same normalisation; result copied back into the image buffer.

Because `tap_index_map` (`FUN_180016a30`) only clamps indices (`map[i] = clamp(i,0,w−1)`),
there is **no rotation and no per-orientation kernel**. The 11 kernels are therefore a family of
**isotropic ridge-scale (width) smoothing profiles**; `cfg[0]` selects one.

Consequences for a port that assumed "11 orientation filters + per-pixel argmax":
- only one kernel is ever applied (the table has a single xref);
- the profiles are all-positive (sum 32768), i.e. low-pass, not band-pass;
- so the "argmax over 11 responses" orientation map does not exist in the vendor.

Mode 1 (`FUN_180016ff0`, uses `0xd7330` two 5×5 kernels + `0x59bbe`) and mode 3
(`FUN_180010f20`) are alternative denoisers selected by `cfg[8]`; they also do not form a bank.

---

## 5. Q3 — Orientation computation (the important one)

After the bank, `engine_process_image` (default `cfg[10]==0`) computes a **structure tensor**:

```c
// FUN_180017390 mode 0
scale = (cfg[0] * 15) / 100;                 // default: 16*15/100 = 2
directional_derivative(img, Ix , 1,0, scale); // FUN_180017160 (smooth x, deriv y)
directional_derivative(img, Iy , 0,1, scale);
directional_derivative(Ix , Ixx, 1,0, scale);
directional_derivative(Iy , Iyy, 0,1, scale);
directional_derivative(Ix , Ixy, 0,1, scale);
```
`directional_derivative` (`FUN_180017160`) builds a 1-D smoothing kernel `[(s/2+1536)/s, (s/2+5120)/s, (s/2+1536)/s]`
(≈ `[1,3.3,1]`) and a derivative kernel `[−0x4000, 0, 0x4000]` and applies them separably via `conv2d`.

`hessian_det_response` (`FUN_180017710`) then computes the blob/ridge response
`det = (Ixx*Iyy − Ixy² + 7) >> 4` and stores a **polarity byte** = `sign(Ixx)`.

**Per-pixel orientation** uses `atan2_deg_lut` (`FUN_180017eb0`) — the `0xd7030` LUT:

```
atan2_deg_lut(cx, cy):                     # returns degrees [0,360)
    ax=|cx|, ay=|cy|
    if ay==0: return cx>0 ? 90 : 270
    if ax==0: return cy>0 ? 0 : 180
    if ax>ay: t = 720 - LUT[(ax/2 + ay*128)/ax]
    else:     t = LUT[(ay/2 + ax*128)/ay]
    quadrant-correct with signs of (cx,cy), then return (t+4)>>3
```
(LUT index uses rounding `+half`, then `/`, clamp 0..128.) This is the vendor's `720−x` atan2
transform noted in the EH575 write-up, but the table is at **`0xd7030`**, not `0xe8006`.

**Per-feature dominant orientation** is computed by `orientation_histogram` (`FUN_180017f80`),
called from `build_feature_descriptors`:

```
# 11x11 window, spacing = param_3 px, centred on the feature
for gy in -5..5, gx in -5..5:
    w = pyramid[gy*11+gx]                  # table 0xd7140
    A[k] = w * Ix(y,x);  B[k] = w * Iy(y,x)     # weighted gradients
bins = 360 >> shift
for k:
    ang = atan2_deg_lut(B[k], A[k])
    bin = (short)(ang >> shift)
    histX[bin] += A[k];  histY[bin] += B[k]     # 2-channel accumulation
# then: windowed (circular) vector sum of histX/histY over `bins`,
#       squared magnitude sum-of-squares, argmax -> dominant bin; also writes
#       feature[+0xe] = atan2_deg_lut(sumX, sumY)  (refined angle)
```

So the orientation is **response-weighted** (weighted by `0xd7140`), derived from the
**structure-tensor gradients via the atan2 LUT**, and the descriptor's main orientation is the
**dominant bin of that weighted histogram** (refined to the accumulated vector's atan2).
It is **not** a plain argmax over the FIR bank.

Separately, `quality_score` (`FUN_180014720`) builds a per-pixel orientation map,
`angle = atan2_deg_lut(gy,gx)` wrapped to ±180°, histograms it in 15° bins per block, and turns
coherence into a quality byte (used for capture acceptance, not the descriptor).

**How orientation is consumed to build the descriptor**: `build_feature_descriptors`
(`FUN_180018af0`) takes the dominant angle, reads `cos[angle]`/`sin[angle]` from `0xca0a0`/`0xd19a0`,
and passes them to `pack_binary_descriptor` (`FUN_180018960`), which rotates the sampling grid
so the descriptor is orientation-normalised.

---

## 6. Q4 — Descriptor construction

Two coupled stages, both in `FUN_180018af0`:

**(a) Orientation histogram / per-bin record** — `FUN_180017f80` produces, per feature, a list of
bins each holding `(sumX, sumY, sumX²+sumY²)` (12-byte records), plus the dominant orientation.
`pyramid[0xd7140]` supplies the 11×11 spatial weights. This is the "descriptor pyramid".

**(b) Binary descriptor** — `pack_binary_descriptor` (`FUN_180018960`):

```
cosA,sinA = cos/sin[feature.orient]        # 0xca0a0 / 0xd19a0
bit = 0
for n in {2,3,4}:                          # three block-grid scales
    vals = block_means_grid(cosA,sinA, n, ...)   # n*n rotated block means,
                                                 # bilinear-sampled from the 3 channels
    for channel in 0..D-1:                       # D = cfg[3] (default 3) channels
        for i in 0..n*n:
            for j in i+1..n*n:
                set bit(bit++) = (vals[channel][j*n? + ...] < vals[channel][i])
                # LSB-first packing into feature+0x10
```

- `block_means_grid` (`FUN_180018380`) samples an **n×n grid rotated by the main orientation**
  (cos/sin), using **bilinear interpolation** of the channel images, and writes per-block means
  (1, 2 or 3 channels depending on `D=cfg[3]`).
- Bit count = `D · Σ_{n=2,3,4} C(n²,2)` = `D · (6+36+120)` = `D · 162`. With default `D = cfg[3] = 3`
  → **486 bits ≈ 61 bytes** (stored at `feature+0x10`, feature stride `0x50 = 80` bytes).
- For Hamming comparison the code rounds the descriptor up to whole 64-bit words:
  `words = cfg[3]<3 ? (cfg[3]==2?6:3) : 8` → type 3 = **8 words / 64 bytes / 512-bit bucket**
  (which is why a "512-bit" port is close; the true payload is 486 bits).

There is **no separate 4×4×32-bin "region" layout** in the vendor path; the EH575 port's
"4×4 regions × 32 bins" is a reconstruction, not the EH57E layout.

Binarisation: it is a **pairwise rank/order comparison** (`a < b`) of the rotated block means,
packed LSB-first — i.e. a BRIEF/rank-style binary descriptor, not a threshold on the histogram.

---

## 7. Q5 — Matching / scoring

Top-level compare `compare_descriptors` (`FUN_18000ac90`):

- **Hamming NN** over the packed words via `POPCOUNT(a ^ b)`, descriptor word count
  `words = 3/6/8` by type (see Q4).
- **Orientation pre-filter**: match candidates are compared only if `feature[+0xd]` (coarse
  orientation bucket) agrees.
- **Lowe-like ratio test**: nearest/second-nearest accepted when
  `nearest*1024 < second*cfg[0x18]`, `cfg[0x18] = 700` → **ratio 0.7**.
- **Hamming budget**: `local_198 = (cfg[0x68]*cfg[0x64] + 0x200) >> 10`, with defaults
  `cfg[0x68]=0x73=115`, `cfg[0x64]=0x400=1024` → **115/512 budget**.
- **Geometric verification**: `verify_matches_geom` (`FUN_180009ab0`) re-projects each candidate
  through the current alignment (`cos/sin` from `0xca0a0/0xd19a0`), re-extracts the descriptor at
  the transformed position (`FUN_180018960`), recomputes Hamming, and keeps it only if within the
  budget (positions within 10 px, orientation within a configured angle, then a reprojection box
  test using `cfg` translation terms).
- **Translation/angle histogram alignment**: `translation_hist_align` (`FUN_18000a1b0`) builds
  360-bin histograms of x/y translation and angle differences, iteratively picks the mode offset,
  marks matched features (`0x8000` flag), subtracts unmatched, and repeats up to
  `cfg[0xa4]*2` / `cfg[0x98]` iterations.
- **Score**: accumulated per matched pair
  `score += (local_184 − hamming)` where `local_184 = 128` for verify (32 for a reduced mode),
  with a floor term `if (0 < cfg[0xf4] && d < cfg[0xf4]*2) d = cfg[0xf4] + d/2;`.
  `local_1b8/sStack_1b6` track the per-template hit score/count.
- **Baked-in thresholds** (default config `DAT_1800e2830`):
  `cfg[0x1d]=0x294=660` (match/score threshold), `cfg[0x1a]=0x73=115` (Hamming budget),
  `cfg[0x12]=0x200=512` (max features), `cfg[0x18]=700` (ratio test), `cfg[1]=1000`
  (interest-point response threshold), `cfg[0x1e]=5` (min matched features).

These are the values the EH575 write-up already quoted (budget 115, threshold 660).

---

## 8. Q6 — `sensor_cfg 0x40a34`, `pyramid 0xd7140`, `atan2 0xe8006`

- **`0xd7140` (121 i32, peak 425)** — the **11×11 descriptor-window weighting kernel**, used in
  `orientation_histogram` (`FUN_180017f80`) with stride `0x2c` (= 11 × i32). Confirmed by the
  code: `w = *(int*)(&DAT_1800d7140 + (row*11 + col)*4)`. It is a spatial (Gaussian-like) window
  over the descriptor patch, symmetric, peak 425.
- **`0xe8006` (128 u16 "atan2")** — **REFUTED.** `0xe8006` is inside the `.reloc` section
  (`.reloc` VA `0xe8000`), it is relocation data, not a LUT. The real atan2 LUT is `0xd7030`.
- **`0x40a34` (sensor_cfg candidate)** — **REFUTED as a code table.** It has:
  - no RIP-relative xref anywhere (annotated-target scan of `.text`),
  - no displacement-form access (`0x40a34(%reg)`),
  - no 64-bit pointer to it anywhere in the file.
  It is most likely incidental `.rdata` (bytes that happen to include 70/57). The engine's real
  configuration is the **heap struct `DAT_1800e2830`**, built by `config_defaults` (`FUN_18000fe70`)
  then `config_by_sensor` (`FUN_180015320`, a `switch(DAT_1800e28e0)` per-sensor-type table). The
  fields that matter: resample scale `[5]`(=`0x700`→1.75), descriptor type `[3]`(3), padding
  `[0x10]`, FIR mode `[8]`(2), norm `[6]`(1), enhance `[7]`(2), kernel selector `[0]`(0x10),
  max features `[0x12]`(512), Hamming budget `[0x1a]`(115), match threshold `[0x1d]`(660).
  So the oracle is correct that `0x40a34` is **not** an input-geometry vector; the geometry comes
  from the runtime config (`cfg[5]` resample + sensor dims from the WBF capture).

---

## 9. What I could not fully pin down (flagged)

1. **Exact per-feature grid spacing / descriptor bins.** `FUN_180018af0` passes a packed
   `param_5 = type<<16 | (cfg[0xf]/10) | (shift<<16)`; the exact `shift` (hence histogram bin count)
   and the `0xf`/`0x10`-derived spacing were not fully traced. The 11×11 window and the pairwise
   descriptor structure are certain; the precise bin count per configuration is not.
2. **Channel identity.** `block_means_grid` samples 3 channels (`D=3`); which of the structure-tensor
   images (`Ix`, `Iy`, `Ixx`, …) each channel is, and the exact channel ordering, is inferred
   (Ix/Iy appear to be `param_2[9]/[10]`) but not byte-for-byte proven.
3. **Mode-1 filter details.** `fir_dispatch` mode 1 uses `0xd7330` (two 5×5 kernels) and the
   `0x59bbe` byte table; I decompiled the dispatch but not the full mode-1 inner loop.
4. **`0x18000fe70` default `cfg[8]`=1 vs `0x180015320` `cfg[8]`=2.** `config_by_sensor` writes
   `cfg[8]=2` after `config_defaults`, so mode 2 is the effective default, but the sensor-type
   switch can override (some cases set mode-1 aux fields). The EH57E sensor-type value
   (`DAT_1800e28e0`) was not resolved from the registry blob.
5. **Score/verdict plumbing.** The final verdict (score ≥ `cfg[0x1d]`) is applied by the engine
   method that calls `compare_descriptors`; I did not enumerate the full WBF interface method
   table (`DAT_1800de030`) to name it, beyond confirming the chain.

## 10. Practical implication for the port

Replace the "11-orientation FIR bank → per-pixel argmax" front-end with:

1. resample ×1.75 (Q10 `cfg[5]`) → normalise (`cfg[6]`) → enhance (`cfg[7]`);
2. one separable ridge-scale smoothing kernel selected by `cfg[0]`;
3. structure tensor (`Ix,Iy,Ixx,Ixy,Iyy`) at scale `cfg[0]*15/100`;
4. response `det = Ixx*Iyy − Ixy²`;
5. interest points = strict 3×3 maxima of `det` + density NMS + sub-pixel Hessian fit;
6. main orientation = **weighted orientation histogram** over the 11×11 `0xd7140` window using
   `atan2_deg_lut` (`0xd7030`), dominant bin vector refined via atan2;
7. binary descriptor = pairwise comparisons of orientation-rotated n×n block means, n∈{2,3,4},
   default 3 channels → 486 bits (packed to 512 for Hamming);
8. matching = orientation-gated Hamming NN, ratio 0.7, budget 115/512, geometric reprojection,
   translation/angle vote, `score = Σ(128 − hamming)`, verdict at 660.

This should recover the vendor's orientation behaviour and fix the ~0.958 margin.

---

### Artifacts
- Ghidra project: `/tmp/opencode/ghidra_proj/Eh57e`
- Scripts: `/tmp/opencode/ghidra_scripts/{FindTables,Decompile2..6}.java`
- Decompiler dumps: `/tmp/opencode/ghidra_decomp{1..6}.txt`, raw logs `/tmp/opencode/ghidra_run*.log`
- Full disassembly (pre-existing): `/tmp/opencode/engine.asm`
