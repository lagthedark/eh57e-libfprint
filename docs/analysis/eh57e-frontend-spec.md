# EH57E front-end — implementable spec (follow-up to §9 of eh57e-frontend-reverse.md)

All addresses are VAs (image base `0x180000000`). Decompiler dumps: `/tmp/opencode/ghidra_decomp2.txt`
(`FUN_180017f80`, `FUN_180018960`, `FUN_180018380`, `FUN_180018af0`), `ghidra_decomp3.txt`
(`FUN_180016ff0`, `FUN_180016be0`, `FUN_180015320`), `ghidra_decomp5.txt` (`FUN_18000fe70`),
`ghidra_decomp7.txt` (`FUN_180021a90`, `FUN_1800060f0`, `FUN_180005150`).

---

## 1. Descriptor geometry, packed `param_5`, histogram bins/shift

### 1a. Where `param_5` is built — `build_feature_descriptors` `FUN_180018af0`

```c
// FUN_180018af0, prologue (ghidra_decomp2.txt:1890-1931)
iVar1  = *(int *)(param_3 + 0x10);          // cfg index 4  (byte 0x10)  -> default = 9
uVar21 = *(int *)(param_3 + 0x3c) / 10;     // cfg index 0xf (byte 0x3c)  -> default = 648/10 = 64
uVar24 = *(int *)(param_3 + 0x3c) % 10;     //                              648%10      = 8
if (uVar21 == 0) uVar21 = 0x40;             // guard
uVar12 = 0;                                 // shift = number of halvings until (uVar24>>k) <= 1
for (uVar5 = uVar24; 1 < uVar5; uVar5 >>= 1) uVar12++;   // 8 -> 4 -> 2 -> 1  =>  uVar12 = 3
...
iVar6 = FUN_180017f80((short *)piVar18, param_2,
                      (6 < iVar1) + 1,                        // <-- descriptor grid SPACING
                      (longlong)pvVar9,
                      uVar24 * 0x100 | uVar21 | uVar12 << 0x10,   // <-- packed param_5
                      param_3);
```

So for the default config:

| field | expr | default value |
|---|---|---|
| `spacing` = `param_3` of `FUN_180017f80` | `(6 < cfg[4]) + 1` | `cfg[4]=9` → **2 px** |
| `param_5` low byte (`uVar21`) | `cfg[0xf]/10` | **64** |
| `param_5` byte 1 (`uVar24`) | `cfg[0xf]%10` | **8** (not used by `FUN_180017f80`) |
| `param_5` byte 2 (shift, `uVar12`) | halving count of `cfg[0xf]%10` | **3** |
| packed `param_5` | `(3<<16) | (8<<8) | 64` | **`0x00030840`** |
| `cfg[0xf]` default | `FUN_18000fe70`: `puVar1[0xf]=0x288` / `FUN_180015320`: `*(u64*)(puVar1+0xf)=0x288` | **`0x288` = 648** |

### 1b. Orientation histogram patch — `orientation_histogram` `FUN_180017f80`

```c
// window extents (ghidra_decomp2.txt:1098-1108)
uVar20    = param_3 + (ushort)((short)param_3 << 2);   // = 5 * spacing = 10 px  (half-extent)
for (gy = center.y - uVar20; gy <= spacing*5 + center.y; gy += spacing)   // 11 rows
  for (gx = center.x - uVar20; gx <= spacing*5 + center.x; gx += spacing) // 11 cols
      w = pyramid[gy_idx*11 + gx_idx];        // table 0xd7140, 121 i32
      A[k] = w * Ix(gy,gx);  B[k] = w * Iy(gy,gx);   // k = valid samples only
```
→ **patch = 11×11 samples, spacing 2 px, half-extent ±10 px (21×21 px patch)**, weights from
`0xd7140`, out-of-bounds samples skipped (count = number of in-bounds samples).

Histogram decode (ghidra_decomp2.txt:1124-1131):
```c
uVar7  = param_5 & 0xff;                       // 64
param_5._0_1_ = (byte)(param_5 >> 0x10);       // shift = 3
local_46c = 0x168 >> shift;                    // bins  = 360 >> 3 = 45
iVar17    = (int)uVar7 >> shift;               // offset = 64 >> 3 = 8
uVar20    = iVar17 + local_46c;                // allocated entries = 8 + 45 = 53
```
Per-sample binning (ghidra_decomp2.txt:1137-1145):
```c
ang = atan2_deg_lut(B[i], A[i]);               // first arg = weighted Iy, second = weighted Ix
bin = (short)(ang >> shift);                   // ang>>3 -> 0..44
histX[bin] += A[i];                            // sum of pyramid-weighted Ix
histY[bin] += B[i];                            // sum of pyramid-weighted Iy
```
Dominant orientation (ghidra_decomp2.txt:1151-1226): the first `offset`(=8) bins are duplicated to
the tail (`memmove(h+45, h, 8*4)`), then a **sliding window of width `offset+1 = 9` bins** scans the
circle; the window whose vector sum `(ΣhistX, ΣhistY)` has the **maximum `((ΣX+0x1000)>>13)² + ((ΣY+0x1000)>>13)²`**
wins. The function returns the winning window's start bin (`argmax`), and writes
`feature[+0xe]` (short) = `atan2_deg_lut(ΣX_best, ΣY_best)` (refined angle, degrees 0..359).

Implementable summary:

```
shift   = 3
bins    = 360 >> shift = 45
offset  = (cfg[0xf]/10) >> shift = 8
window  = offset + 1 = 9 bins
spacing = (6 < cfg[4]) + 1 = 2 px
patch   = 11 x 11 samples at spacing, half-extent 5*spacing
feature.orient = atan2_deg_lut(Sx, Sy) over the winning 9-bin window
dominant_bin   = start bin of the winning window   // returned by FUN_180017f80
```

> Note: `cfg[0xf]=648` makes `bins=45`, **not 32**; the port's `4×4×32-bin` layout is not the vendor.

---

## 2. Channel identity / order in `block_means_grid` (`FUN_180018380`) — byte-for-byte

Call chain proving the channels:

```c
// FUN_180018af0 (ghidra_decomp2.txt:1887-1891)
piVar18  = **(param_1 + 6);   // feature array
local_68 = param_2[5];        // <-- param_2 == plVar10 (image context)
local_60 = *param_2;          //     plVar10[0]
local_58 = param_2[1];        //     plVar10[1]
...
// FUN_180018960((longlong)piVar18, &local_68, x, y, cfg);   // 2nd arg = &local_68
//   inside FUN_180018960:  FUN_180018380(out, n, x, y, cos, sin, /*param_7=*/param_2 /*=&local_68*/, cfg)
```
Hence inside `FUN_180018380`, `param_7` points at the 3-slot stack array
`{ local_68, local_60, local_58 } = { plVar10[5], plVar10[0], plVar10[1] }`.

What those `plVar10` slots are (from `FUN_180018f30` / `FUN_180017710` / `FUN_180017390`):

| slot | set by | meaning |
|---|---|---|
| `plVar10[5]` (`+0x28`) | `engine_process_image`, then filtered in place by `FUN_180016ff0` | **FIR-smoothed 32-bit intensity image** |
| `plVar10[0]` (`+0x00`) | `FUN_180017390` (mode 0) | **Ix** (∂x) |
| `plVar10[1]` (`+0x08`) | `FUN_180017390` (mode 0) | **Iy** (∂y) |
| `plVar10[2]`,`[3]`,`[4]` | `FUN_180017710` frees `[2],[4]`, zeroes `[2..4]` | Ixx/Ixy/Iyy, **freed** |
| `plVar10[6]` (`+0x30`) | `FUN_180017710` | det = Ixx·Iyy − Ixy² (interest-point response) |

In `FUN_180018380` (ghidra_decomp2.txt:323, 377-400):
```c
piVar1 = (int *)*param_7;                    // = plVar10[5]  -> smoothed image (ch0)
lVar2  = rowptr(param_7[1]);                 // = plVar10[0]  -> Ix            (ch1)
lVar3  = rowptr(param_7[2]);                 // = plVar10[1]  -> Iy            (ch2)
...
iVar15 = bilinear(ch0);   // accumulated into iVar12 (sum of ch0)
iVar30 = bilinear(ch1);   // accumulated into iVar11 / iVar11
iVar7  = bilinear(ch2);   // accumulated into iVar16
```
Output per block (ghidra_decomp2.txt:451-471), `count = iVar9`:
```c
// D = cfg[3]
if (D == 3) {
    // rotate (sumX=Σch1, sumY=Σch2) into the feature's main orientation:
    sumX = iVar11; sumY = iVar16;
    local_78[-2] = iVar12 / count;                              // mean of ch0 (smoothed intensity)
    local_78[-1] = (( (sumX+8)>>4 * cos ) + ( (sumY+8)>>4 * sin ) + 0x800) >> 12 / count;   // rot gx
    *local_78    = (( (sumY+8)>>4 * cos ) - ( (sumX+8)>>4 * sin ) + 0x800) >> 12 / count;   // rot gy
}
// D == 2: local_78[-2]=mean ch0 ; local_78[-1] = Σ(ch1²+ch2²) / count
// D == 1: local_78[-2]=mean ch0
```

**Conclusion (exact):** with `D = cfg[3] = 3`, the three descriptor block values are, per block:
- **channel 0** = mean smoothed intensity (`plVar10[5]`),
- **channel 1** = orientation-rotated block-mean of **Ix** (`plVar10[0]`),
- **channel 2** = orientation-rotated block-mean of **Iy** (`plVar10[1]`).

The orientation used for the rotation is `feature[+0xe]` (the dominant angle from §1), and
`cos/sin` are read from `0xca0a0`/`0xd19a0`.

Grid geometry (`FUN_180018380`, ghidra_decomp2.txt:325-348):
```c
dVar31 = (double)cfg[4];                     // 9
start  = cfg[4]*-0x400 + -0x200;             // -9728   (= -9.5 px in 1/1024 units)
step   = (int)(((2*cfg[4] + 1.0) / n) * 1024.0);   // (19/n) px, n = 2,3,4
```
→ **n×n block centres at ±(19/n) spacing starting at −9.5 px** (i.e. n=2: −9.5/0.0; n=3: −9.5/−3.17/+3.17;
n=4: −9.5/−4.75/0/+4.75), each block averaged over a rotated cell of side ≈`step`, rotated by the
feature orientation via `cos/sin`, bilinear-sampled from the 3 channels.

---

## 3. `fir_dispatch` mode 2 — exact kernel selection and normalisation

`FUN_180016ff0` (ghidra_decomp1.txt:902-970), mode `= cfg[8] == 2`:

```c
// stack byte table (cumulative i32 offsets of the 11 kernels in 0xd6ed0):
local_38[0..7] = {0,5,10,0x0f,0x16,0x1d,0x24,0x2d};   // and local_30.. = {0x36,0x3f,0x4a}
//  == [0,5,10,15,22,29,36,45,54,63,74]
iVar2 = 10;
if (*param_2 + -10 < 0xb) iVar2 = *param_2 + -10;      // idx = min(cfg[0]-10, 10)
uVar4 = FUN_180016be0(*(int **)(param_1 + 0x28),
                      (int *)(&DAT_1800d6ed0 + (ulonglong)local_38[iVar2] * 4),
                      (uint)(byte)(&DAT_180059bc8)[iVar2]);   // length table
```
- `cfg[0]` default = `0x10` (16) → **idx = 6**.
- `local_38[6] = 0x24 = 36` i32 → kernel at `0xd6ed0 + 0x90 = 0xd6f60` = kernel #6 =
  `[361,1415,3757,6748,8205,6749,3757,1415,361]` (**9 taps**), length `0x59bc8[6] = 9`.
- Confirmed: `idx = min(cfg[0]-10, 10)`; 9-tap kernel #6 for the default `cfg[0]=0x10`.

Two-pass separable normalisation (`FUN_180016be0`, ghidra_decomp2.txt:148, 184):
```c
*(int *)(out_row + col*4) = (sum + 0x4000) >> 0xf;   // == round(sum / 32768), both passes
```
Pass 1 = horizontal 1-D dot product with `tap_index_map(width, len)` (identity + edge-clamp via
`FUN_180016a30`); result copied back to the image; pass 2 = same kernel applied vertically
(`tap_index_map(height, len)`), same normalisation. **Confirmed `(sum + 0x4000) >> 15`, same kernel
both axes.**

---

## 4. `atan2_deg_lut` (`FUN_180017eb0`) — bit-faithful formula

Table `L[0..127]` (u16) at RVA **`0xd7030`** = `[0,4,7,11,14,18,21,25,29,32,...,356,358]`
(`L[i] ≈ round(8·atan(i/128)·180/π)`; strictly non-decreasing, max 358).

Decompiled body (ghidra_decomp1.txt:977-1026), reproduced exactly:

```c
int atan2_deg_lut(uint cx, uint cy) {   // arg1 = cx, arg2 = cy
    uint ay = abs(cy), ax = abs(cx);
    if (ay == 0) return (cx > 0) ? 0x5a : 0x10e;          // 90 : 270
    if (ax == 0) return (cy > 0) ? 0 : 0xb4;              // 0  : 180
    uint t;
    if (ax > ay) {                                        // note: strict >, else branch takes ax<=ay
        uint idx = (ax/2 + ay*0x80) / ax;                 // floor((ax/2 + 128*ay)/ax)
        t = 0x2d0 - L[idx];                               // 720 - L[idx]
    } else {
        uint idx = (ay/2 + ax*0x80) / ay;
        t = L[idx];
    }
    if ((int)cy < 1) {                                    // cy <= 0
        if ((int)cx < 0) return (int)(t + 0x5a4) >> 3;    // (t + 1444) >> 3
        t = 0x5a0 - t;                                    // 1440 - t
    } else {                                              // cy > 0
        if ((int)cx >= 0) goto L60;                       // -> (t+4)>>3
        if ((int)t < 5) return 0;
        t = 0xb40 - t;                                    // 2880 - t
    }
L60:
    return (int)(t + 4) >> 3;
}
```

Notes for a port:
- integer division truncates; the `+half` is folded into the numerator (`ax/2`, `ay/2`).
- LUT index is in `[0,128)` (never 128 because `ay<ax` / `ax<=ay` strictly, and the `+half` can only
  reach 128 when `ay==ax` and `ax/2` rounding… guard `idx` to `<=127` if you observe 128; the vendor
  table has 128 entries and the arithmetic produces `idx<=127` for the shown branches).
- Result is degrees in `[0,360)`; `cx` is the “cos-like” axis (0° at `cy=0,cx>0` → 90 is returned for
  `+cx`; `+cy` → 0). Reproduce exactly, don't “fix” the ±1 asymmetry.

---

## 5. `config_by_sensor` (`FUN_180015320`) — sensor type and defaults

**Sensor type is runtime.** `FUN_180021a90` (engine init) does (ghidra_decomp7.txt:133-135):
```c
uVar1 = (**(code **)(*param_1 + 0x98))(param_1);          // WBF sensor-object virtual method
thunk_FUN_180004c00(param_1[0x32], *(uint *)(param_1 + 0x33), uVar1, 0);   // param_3 = uVar1
// FUN_180004c00:  DAT_1800e28e0 = (undefined4)param_3;  FUN_180015320();
```
Plus a registry override `LOCK_SENSORID` (DWORD) read from
`HKLM\...\<device>\LOCK_SENSORID` (also via the sensor object's `vtable+0xa0` name). There is **no
EH57E-specific constant** in the engine DLL: a search for a `70,57` / `57,70` pair (any width)
found nothing, and the geometry is obtained from the sensor at runtime.

**Default-config path** (what applies when `FUN_180015320`'s `switch(DAT_1800e28e0)` hits `default`,
and the preamble is always applied first):

`FUN_18000fe70` allocates the struct and sets hard defaults; then `FUN_180015320` overwrites the
common preamble. Effective values (cfg index → value) for the **base/default** path:

| cfg idx | byte | value | meaning |
|---|---|---|---|
| `0` | 0x00 | `0x10` (=16) | FIR kernel selector (`idx = min(cfg0−10,10)` → 6) |
| `1` | 0x04 | `1000` | interest-point response threshold |
| `3` | 0x0c | `3` | descriptor channels D (486 bits) |
| `4` | 0x10 | `9` | descriptor patch scale → spacing `(6<9)+1 = 2` |
| `5` | 0x14 | `0x700` (=1792) | resample factor Q10 → **1.75×** |
| `6` | 0x18 | `1` | normalisation mode |
| `7` | 0x1c | `2` | enhancement mode (CLAHE-like) |
| `8` | 0x20 | `2` | FIR dispatch mode |
| `0xf` | 0x3c | `0x288` (=648) | orientation-histogram control → bins 45, shift 3, offset 8 |
| `0x12` | 0x48 | `0x200` (=512) | max features |
| `0x16` | 0x58 | `1` | `block_means_grid` bilinear flag |
| `0x1a` | 0x68 | `0x73` (=115) | Hamming budget |
| `0x1d` | 0x74 | `0x294` (=660) | score/match threshold |
| `0x18` | 0x60 | `700` (from `FUN_18000fe70`) | ratio-test (nearest·1024 < second·700) |

The `switch(DAT_1800e28e0)` (ghidra_decomp5.txt:124-438) covers types
`5,7,8,9,0xb,0xe,0x10,0x11,0x12,0x13,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e/0x1f,0x20,0x21`
(and mirrored `0x87,0x88,0x89,0x93,0x95,0x99,0x9a,0x9d`). EH57E's numeric type comes from the WBF
sensor object at runtime; it is **not** resolvable from the engine alone. Example cases:
- `0x10`/`0x14`: `cfg[5]=0x467`, `cfg[1]=0x4b5`, `cfg[0x18]=0x3aa`, `cfg[0x1a]=0x7f`, `cfg[0x1b]=8`, `cfg[0x1c]=7`
- `0x1d`: `cfg[0]=0xd`, `cfg[1]=0x348`, `cfg[2]=2`, `cfg[5]=0x744` (1.816×), `cfg[0xd]=400`, `cfg[0x1a]=0x70`, `cfg[0x1d]=600`, `cfg[0x10]=2`
- `0x20`: `cfg[1]=0x4b5`, `cfg[5]=0x467`, `cfg[0x18]=1000`, `cfg[0x1a]` etc.
- Unknown type → `FUN_180015320` returns `0xfffff89f`/`-0x3ea`, but the preamble (default row above) stays in effect.

For a static EH57E port, use the **default row** above (it is exactly the `FUN_18000fe70`+preamble
result), unless you can read the sensor type on device.

---

## 6. Exact things a faithful C port must reproduce

### 6a. `block_means_grid` (`FUN_180018380`) bilinear sampler
- Coordinates are **16.16 fixed point**. Integer part `ix = X>>16`, fraction `fx = X - (ix<<16)`
  (equivalently `X & 0xffff`), `1-fx = 0x10000 - fx`; same for `y`.
- **Edge clamp** of the +1 sample: `ix1 = min(ix+1, w-1)`, `iy1 = min(iy+1, h-1)` (replication,
  not zero).
- Two **separate 16-bit truncations** — this is not a single 32-bit bilinear:
  ```
  // channel 0 (with cfg[0x16]==1 bilinear path):
  top = ( fx*img[iy1][ix1] + (0x10000-fx)*img[iy1][ix] ) >> 16;
  bot = ( (0x10000-fx)*img[iy][ix] + fx*img[iy][ix1] ) >> 16;
  val0 = ( top*fy + bot*(0x10000-fy) ) >> 16;
  ```
  (channels 1,2 use the same structure; the accumulator roles of `fx/fy` are swapped between the
  two vertical terms in the decompile — reproduce the exact expression from
  ghidra_decomp2.txt:385-400.)
- Bilinear path is selected by `cfg[0x16]==1` (default). The else branch is nearest
  (`(X+0x7fff)>>16`).
- Only pixels with `0 <= ix < w && 0 <= iy < h` contribute; out-of-bounds samples set a flag byte
  and are excluded from the average (`count` = number of valid samples; if `count < 12` the whole
  grid returns `-1` → feature rejected).

### 6b. `pack_binary_descriptor` (`FUN_180018960`) bit ordering — exact
```c
bit = 0;
for (n = 2; n <= 4; n++) {                       // order: n=2, then 3, then 4
    n2 = n*n;
    block_means_grid(vals, n, x, y, cos, sin, channels, cfg);   // vals[c + k*D], D = cfg[3]
    D  = cfg[3];                                  // == 3
    for (c = 0; c < D; c++)                       // channel inner to n
        for (i = 0; i < n2; i++)
            for (j = i+1; j < n2; j++) {
                int a = vals[c + i*D];            // earlier block
                int b = vals[c + j*D];            // later block
                if (b < a)                        // comparison direction: later < earlier
                    feature[0x10 + (bit>>3)] |= (1 << (bit & 7));   // LSB-first packing
                bit++;
            }
}
```
- **stride** between blocks = `D = cfg[3]` ints; channel offset = `c`.
- **comparison direction** = `vals[j] < vals[i]` (strict `<`, no swap).
- **bit order** = LSB-first within each byte (`1 << (bit & 7)`), bytes appended from `feature+0x10`.
- **bit count** = `D · Σ_{n=2,3,4} C(n²,2)` = `3·(6+36+120)` = **486 bits** = 61 bytes
  (`(cfg[3]*162+7)>>3`); Hamming rounds up to `8 qwords = 64 bytes` (3 pad bytes must stay 0,
  which calloc guarantees).
- Per-block value layout written by the grid routine is `vals[block*D + c]` (block = row-major
  `r*n+c`).

### 6c. Orientation histogram exactness
- weight = `pyramid[gy*11+gx]` from `0xd7140`; samples out of image bounds are skipped, not zeroed.
- angle = `atan2_deg_lut(w*Iy, w*Ix)` (first arg = weighted Iy, second = weighted Ix).
- bin = `angle >> 3` (0..44); accumulate raw weighted sums `Σ(w·Ix)`, `Σ(w·Iy)` per bin.
- dominant = 9-bin sliding window (offset 8) on the **circular** histogram (duplicate bins 0..7 to
  45..52), argmax of `((ΣX+0x1000)>>13)² + ((ΣY+0x1000)>>13)²`; feature angle = `atan2_deg_lut(ΣX,ΣY)`
  of the winning window.

### 6d. Feature struct (stride 0x50 = 80 bytes)
`+0x00` x, `+0x04` y (16.16 → `>>16`), `+0x08` magnitude, `+0x0c` polarity/quality byte,
`+0x0d` coarse-orientation bucket, `+0x0e` orientation (short, degrees), `+0x10..+0x4c`
61-byte binary descriptor. (`FUN_180018af0` advances the feature pointer by `0x14` ints = 80 bytes.)

---

## 7. Remaining uncertainty (small)

- **Channel-1/2 vertical term swap** in the bilinear combination (ghidra_decomp2.txt:385-400): the
  exact `fx`/`fy` assignment for the vertical combine differs between channel 0 and channels 1/2 in
  the decompile; copy the decompiled expressions verbatim rather than the tidy formula.
- **Sensor type numeric value** for EH57E is genuinely runtime (WBF sensor object); only the default
  config row is statically certain.
- `FUN_180017f80` writes per-bin records only when `cfg[0xe] != 0` (default 0 → skipped); if you need
  the `(ΣX,ΣY,ΣX²+ΣY²)` records, set `cfg[0xe]` accordingly.
