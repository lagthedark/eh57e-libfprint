# EH57E — how the vendor resolves the ridge 180° ambiguity

**Short answer:** it does **not** resolve it inside `FUN_180017f80`. The histogram there is a raw,
signed, 360° gradient histogram (first-derivative `Ix/Iy`), which *is* bimodal on a ridge. The vendor
tolerates the ambiguity by (a) generating **up to 3 orientation hypotheses per interest point** in
`FUN_180018af0` (top peaks of that 360° histogram) and building a descriptor for each, and (b) folding
the angle difference to `[0,180]` in the matcher. There is **no** doubled-angle / folded-180 / squared
accumulation anywhere.

Citations use `ghidra_decomp*.txt` (Ghidra 12 headless dumps under `/tmp/opencode`).

---

## 1. What `FUN_180017f80` accumulates — raw signed `w·Ix`, `w·Iy` (no doubling/squaring)

Per-sample accumulation (`ghidra_decomp8.txt:522-527`):
```c
iVar8  = *(int *)(puVar18 + sVar21*4);                 // pyramid weight 0xd7140[gy][gx]
iVar17 = *(int *)(sVar6*4 + lVar4);                    // channel B  (plVar10[1])
auStack_238[uVar24] = iVar8 * *(int *)(sVar6*4 + lVar10);  // A = w * plVar10[0]  (= w*Ix)
auStack_428[uVar24] = iVar8 * iVar17;                  // B = w * plVar10[1]  (= w*Iy)
```
Binning + histogram add (`ghidra_decomp8.txt:548-556`):
```c
uVar7  = auStack_238[sVar19];                 // A
uVar25 = auStack_428[sVar19];                 // B
iVar17 = FUN_180017eb0(uVar25, uVar7);        // atan2_deg(cx=B, cy=A)
lVar10 = (short)(iVar17 >> shift);
histX[bin] += uVar7;                          // + w*Ix   (raw signed)
histY[bin] += uVar25;                         // + w*Iy   (raw signed)
```
→ **raw signed `w·Ix` / `w·Iy`; NOT `A*A−B*B`, NOT `2AB`, NOT `2θ`.** The bin is the sample's own
gradient angle (single angle), not doubled.

## 2. 45 bins over 360° — not folded

`ghidra_decomp8.txt:535-537`:
```c
param_5._0_1_ = (byte)(param_5 >> 0x10);      // shift = 3
local_46c = 0x168 >> shift;                   // bins = 360 >> 3 = 45
iVar17    = (int)uVar7 >> shift;              // offset = (cfg[0xf]/10)>>3 = 64>>3 = 8
uVar20    = iVar17 + local_46c;               // 53 entries
```
`bin = angle >> 3`, `angle ∈ [0,360)` → **45 bins spanning the full 360°**. No `mod 180`, no `2θ`.

(`cfg[0xf]=0x288=648` default → shift 3, bins 45, offset 8.)

## 3. The "9-bin sliding window"

`ghidra_decomp8.txt:562-627`:
```c
memcpy(histX+45, histX, 8*4);   // duplicate bins 0..7 to 45..52  (circular)
memcpy(histY+45, histY, 8*4);
s = -8;
for (k = 0; k < 53; k++) {
    s++;                                   // window start
    sumX += histX[k];  sumY += histY[k];   // running window sum (width 9: [s..s+8])
    if (s >= 0) {
        mag = ((sumX + 0x1000) >> 13)^2 + ((sumY + 0x1000) >> 13)^2;
        if (local_450) local_450[s] = {sumX, sumY, mag};   // per-bin record (12 bytes)
        sumX -= histX[s];  sumY -= histY[s];
        if (mag > best) { best=mag; best_bin=s; bestX=sumX; bestY=sumY; }
    }
}
return best_bin;                                     // local_474
feature[+0xe] = atan2_deg(bestY, bestX);             // if vector nonzero, else -1
```
- window **width 9 bins** (offset+1 = 8+1), start `s` scans all 53 positions (circular via duplicated
  bins 0..7); window covers `[s .. s+8]`.
- selection = `argmax` of the **squared magnitude of the windowed vector sum** `(ΣhistX, ΣhistY)`.
- returned = window **start bin** (`local_474`); stored `feature[+0xe]` = `atan2_deg(ΣY,ΣX)` of the
  winning window (full `[0,360)`).

This is a *windowed vector sum* selection, but with a 72° window it selects one lobe of the bimodal
histogram — it does **not** fold ±180.

## 4. THE resolution: multi-orientation feature generation in `FUN_180018af0`

`FUN_180018af0` runs a **peak-search loop by default** (`ghidra_decomp9.txt:296-356`). Because
`cfg[0x38] = 0x2df ≠ 0` (default from `FUN_18000fe70`), the per-bin records `pvVar9` are allocated and
the loop that emits extra orientation hypotheses runs:

```c
// after FUN_180017f80 returns the dominant bin `lVar20` and wrote feature[+0xe]:
iVar2  = records[lVar20].power;          // power at dominant bin   (record = {sumX,sumY,power})
uVar28 = 0x40 / (cfg[0xf]%10) + 1;       // 64/8 + 1 = 9   -> suppression window width
uVar5  = 3 * n_features;                 // GLOBAL cap: up to 3 hypotheses per interest point
do {
    // 1) suppress a 9-bin window around the current peak (mod local_98=53)
    lVar20 = dominant_bin - 0x20/(cfg[0xf]%10);      // dominant - 4
    for (i=0;i<uVar28;i++) records[(lVar20+i) mod 53].power = 0;
    // 2) find next-highest-power bin above threshold
    thr = (cfg[0x38] * iVar2) >> 10;                 // cfg[0x38]*Pmax/1024  (default 735/1024 ≈ 0.72)
    sVar26 = argmax_bin(records[*].power); if (records[sVar26].power < thr) break;
    // 3) emit a NEW feature at the SAME (x,y) with orientation = that bin's vector
    feature_i[0..0xf] = feature_0[0..0xf];              // same location
    feature_i[+0xe] = atan2_deg(records[sVar26].sumY, records[sVar26].sumX);
    FUN_180018960(feature_i, ...);                      // build a descriptor for THIS orientation
} while (feature_count < uVar5);
```
Key constants:
- suppression window = `0x40/(cfg[0xf]%10) + 1` = **9 bins** (72°);
- window centre offset = `0x20/(cfg[0xf]%10)` = **4 bins**;
- acceptance threshold = `(cfg[0x38]·Pmax) >> 10` — `cfg[0x38]` default **0x2df = 735** (≈0.718·Pmax),
  other sensor cases set it to `cfg[0x1d]` (600–700);
- up to **3 hypotheses per interest point** (global cap `3·n_features`).

So on an ambiguous ridge, if the ±180 lobe's power ≥ threshold it is **also emitted as a second
feature with orientation `θ+180`**, and a descriptor is built for it. The matcher then matches whichever
hypothesis is consistent. When the second lobe is below threshold (ratio < cfg[0x38]/1024), only the
stronger (consistent) lobe is kept. Either way the engine never needs a folded histogram.

**This is the mechanism your single-orientation port is missing.** A faithful port must reproduce this
multi-hypothesis expansion (same location, orientation from each histogram peak), not just the dominant
bin. (If you keep a single descriptor per point, you will see exactly the ~180° bimodal flips you
observed.)

## 5. Corroboration from the matcher

The matcher additionally treats `θ` and `θ+180` as equal when pairing candidates
(`ghidra_decomp2.txt:2817-2832`, `FUN_180009ab0`):
```c
iVar13 = feature[+0xe] - other[+0xe];
sVar8  = abs(iVar13);
sVar9  = 0x168 - sVar8;                 // 360 - |Δ|
if (sVar8 < 0xb5) sVar9 = sVar8;         // fold to [0,180]
```
No other 180-folding exists in the extraction path.

---

## Q4 — `feature[+0xd]` (and `+0xc`)

- `feature[+0xc]` is set to the **ridge-polarity byte** = `sign(Ixx)`:
  - `FUN_180017710` (`ghidra_decomp8.txt:692-694`): `*pbVar5 = (byte)((uint)Ixx >> 31)` written into
    the `plVar10[7]` image, one byte/pixel;
  - `FUN_1800178a0` (`ghidra_decomp2.txt:170-171`) reads that mask into `record[+0xc]`;
  - `FUN_180017be0` copies `+0xc` when refining.
- `feature[+0xd]` is **not written in the extraction path** (records are calloc'd; only `+0xc`, `+0x8`,
  `+0xe` are written). It is consumed by `compare_descriptors` as a **gate**, not a fine orientation:
  `ghidra_decomp6.txt:202`: `if ((char)probe[+0xc] == (char)gallery[+0xd] && gallery[+0xe] >= 0)`.
  I.e. it is a **polarity/type gate** comparing the probe's polarity byte to the gallery's field; it is
  *not* an orientation-bucket gate in this build, and the orientation tolerance is handled by the
  `[0,180]` fold in §5. (The gallery layout appears to carry that byte at `+0xd`, the probe at `+0xc`.)

## Q5 — exact `Ix/Iy`

`FUN_180017390` runs **mode = `cfg[10] = 1`** (preamble of `FUN_180015320` sets index 10 = 1;
`ghidra_decomp5.txt:84`), so `plVar10[0]/[1]` come from `FUN_180011330`, not `FUN_180017160`:

`FUN_180011330` (`ghidra_decomp8.txt:53-65`) computes, per pixel (p = 2):
```
A = src
out1 = ( 3*((A[y+p][x+p]-A[y-p][x-p]) - (A[y+p][x-p]-A[y-p][x+p])) + 10*(A[y][x+p]-A[y][x-p]) + 0x20) >> 6
out2 = ( 3*((A[y+p][x-p]-A[y-p][x-p]) - A[y-p][x+p] + A[y+p][x+p]) + 10*(A[y+p][x]-A[y-p][x]) + 0x20) >> 6
```
Numerically verified: `out1(x)=1`, `out1(x²)=2x+const` (odd ⇒ first derivative), `out2(y)=1`.
⇒ **`plVar10[0] = Ix = ∂A/∂x`, `plVar10[1] = Iy = ∂A/∂y`**, positive sign (increasing intensity → +),
computed on the FIR-smoothed 32-bit image `plVar10[5]`. (`plVar10[2]=Ixx`, `[3]=Ixy`, `[4]=Iyy` are
used only to form the interest-point response `det = Ixx·Iyy − Ixy²` in `FUN_180017710`, then freed.)

---

## Bit-faithful pseudocode (orientation + hypotheses)

```
# per interest point (x,y):
histX[53]=0; histY[53]=0
for gy in -5..5, gx in -5..5:                    # spacing = (6<cfg[4])+1 = 2
    if (x+gx*2, y+gy*2) in image:
        w = PYRAMID[gy+5][gx+5]                  # 0xd7140
        A = w*Ix(y+gy*2, x+gx*2); B = w*Iy(...)
        bin = atan2_deg(B, A) >> 3               # atan2_deg from 0xd7030, [0,360)
        histX[bin]+=A; histY[bin]+=B
histX[45..52] = histX[0..7]; histY[45..52] = histY[0..7]
records[53]; best=-1
for s in 0..52:
    sumX = Σ histX[s..s+8]; sumY = Σ histY[s..s+8]          # width 9
    mag  = ((sumX+0x1000)>>13)^2 + ((sumY+0x1000)>>13)^2
    records[s] = {sumX,sumY,mag}
    if mag>best: best=mag; dominant_bin=s; dBX=sumX; dBY=sumY
feature.orient = atan2_deg(dBY, dBX)             # stored feature[+0xe]

# --- multi-hypothesis expansion (FUN_180018af0) ---
emit feature at (x,y, feature.orient); build descriptor
if cfg[0x38] != 0:
    thr = (cfg[0x38]*records[dominant_bin].mag) >> 10
    while total_features < 3*n_features:
        zero records[(dominant_bin-4+i) mod 53].mag for i in 0..8     # 9-bin suppression
        k = argmax records[*].mag
        if records[k].mag < thr: break
        emit feature at (x,y, atan2_deg(records[k].sumY, records[k].sumX)); build descriptor
```

### Practical fix for the port
- Keep the raw signed 360° histogram exactly as above (it is correct).
- Add the **multi-hypothesis expansion** (up to 3 orientations/point, 9-bin suppression, threshold
  `cfg[0x38]·Pmax/1024`, default `cfg[0x38]=735`).
- Match with the `[0,180]` angle fold.
- Do **not** try to fold/double the histogram itself — the vendor does not.
