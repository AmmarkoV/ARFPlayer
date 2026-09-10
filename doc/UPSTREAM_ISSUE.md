# Root-joint glitch bursts in exported motion (ARF and BVH)

**To:** the SAM3DBody-cpp maintainers
**From:** ARFPlayer (`libarf`), an independent reader of `.arfz` containers
**Repo:** <https://github.com/AmmarkoV/SAM3DBody-cpp>
**Affects:** `ARFWriter` and `BVHWriter` output — i.e. the shared `mhr_fk` core,
not either exporter
**Severity:** cosmetic for CG playback, but it makes exported motion unusable
as-is for anything that consumes joint rotations directly

---

## Summary

Exported clips contain short bursts, typically two to three frames long, where
the **pelvis (`root`, joint index 1) local rotation is roughly 100° off the
smooth trajectory either side of it**. Because the whole skeleton hangs off the
pelvis, the body folds over sideways for those frames.

The bad frames are **not detectable as corrupt data**. The matrices are
orthogonal to 1e-7, determinant +1, unit scale, correctly framed. They are
valid rotations that happen to be wrong. Only their velocity gives them away.

This is not specific to ARF: the same signature is present in the committed
`gmr_out/*.bvh` exports, which come from a different exporter sharing the same
FK. And `knowledge/GMR.md` §7 already describes the symptom ("a root-rotation
flip") and works around it downstream in `tools/gmr_retarget.py`.

---

## Reproduction

Any container or BVH from the pipeline will do. Using the one committed to the
ARFPlayer repo (`samples/summerlove_0.arfz`, 551 frames at 30 fps, 127 joints):

```bash
./build/arfplay --info samples/summerlove_0.arfz
# glitch frames : 16 of 551 exceed 40 deg/frame
```

Play it and the dancer folds flat several times in the first two seconds.

---

## Evidence, in the ARF container

Joint rotation velocity over all 127 joints and all 551 frames:

| statistic | value |
|---|---|
| mean | 1.78 °/frame |
| p99 | 14.17 °/frame |
| max | **155.20 °/frame** |

There are 20 single-frame joint deltas above 60°. **All 20 are the `root`
joint.** No other joint in the skeleton ever exceeds 60° in one frame.

Composed and skinned, the mesh height (median 163 cm) collapses on these
frames:

```
frames with an inverted torso axis : 1, 2, 3, 8, 9, 12, 13, 18, 19, 31, 32, 37, 38, 42, 43
frames whose mesh is under 120 cm  : 1, 3, 8, 9, 12, 13, 18, 19, 31, 32, 37, 38, 42, 43
worst                              : f3 = 50 cm, f1 = 53 cm, f43 = 54 cm, f12 = 65 cm
```

All of them fall inside the first 44 frames of this track. The matrices
themselves are fine:

```
f7 root: scale=1.0000 det=+1.0000 |RRt-I|=2.4e-07     (clean frame)
f8 root: scale=1.0000 det=+1.0000 |RRt-I|=1.2e-07     (glitch frame)
f9 root: scale=1.0000 det=+1.0000 |RRt-I|=1.7e-07     (glitch frame)
```

One detail that looks relevant: **the pelvis local rotation never leaves the
neighbourhood of 180°.** Over frames 0–50 its rotation angle ranges 91°–177°,
sitting at 152°–177° for most of them. It is parked in the region where an
Euler parameterisation is least well conditioned, for the entire clip.

---

## Evidence that this is not an ARF problem

Root angular velocity in the committed BVH exports, decoded per each file's own
`CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation`:

| clip | frames | mean | p99 | max | frames > 40°/fr |
|---|---:|---:|---:|---:|---:|
| `boom/boom_0` | 1442 | 5.7° | 114.8° | **165.7°** | 42 |
| `eyzon/eyzon_0` | 264 | 8.2° | 124.7° | **155.1°** | 13 |
| `eyzon/eyzon_1` | 810 | 2.1° | 23.1° | **155.8°** | 7 |
| `fortnite1/fortnite1_0` | 1721 | 4.2° | 54.1° | **144.7°** | 24 |
| `zeimpekiko/zeimpekiko_0` | 2368 | 2.5° | 22.4° | **144.7°** | 15 |
| `sign/sign_0` | 200 | 0.6° | 3.0° | 3.1° | 0 |
| `eyzon/eyzon_3` | 14 | 2.6° | 4.1° | 4.1° | 0 |

Five of seven clips show it. The two that do not are a 200-frame low-motion
clip and a 14-frame fragment.

`knowledge/GMR.md` independently reports 153 °/frame root angular velocity on
`eyzon_1`; this table measures 155.8° on the same file. Same phenomenon.

---

## What has been ruled out

* **Not a reader bug.** `libarf` round-trips every binary payload of the sample
  byte-for-byte, and its hierarchy composition matches `compose_trs_mat4` and
  `mhr_fk::State::compute` exactly. 536 of 551 frames compose to a correct
  standing human.
* **Not a container/serialisation problem.** The BVH exports have it too.
* **Not one exporter.** `export_to_bvh` and `export_to_arf` both show it, so it
  is upstream of both, in the shared FK or its inputs.
* **Not removed by the existing smoothing.** The pass order in
  `src/render/offline_sam_3dbody_render.cpp` is
  `gap_interpolation_pass` (320) → `interpolate_jitter_pass` (323) →
  `smoothing_pass` (326) → `export_to_bvh` (334) → `export_to_arf` (337).
  Pass 5 ran, with defaults, and the glitches survived it.

---

## Likely mechanism (hypothesis — worth one print statement to confirm)

`mhr_fk.cpp:122-123` builds each joint's local rotation as:

```cpp
euler_mhr_to_quat(jp[3], jp[4], jp[5], q_euler);
qmul(&q_local_[j*4], pre + j*4, q_euler);
```

where `jp[3..5]` are Euler angles from a linear PCA decode of
`mhr_model_params`. Euler→quaternion is continuous, so a ~100° jump in the
resulting rotation requires a comparably large jump in the decoded angles.

The most likely source is an **angle wrap in the Euler domain being smoothed
across**. `smooth_segment` (`offline_passes.cpp`) filters the model parameters
with `filter_channels(mhrp, 204, …)` — a plain linear filter. Joint rotations
are downstream of those coefficients through a linear decode, so filtering the
coefficients is equivalent to filtering the Euler angles directly. If an angle
wraps (+179° → −179°, the *same* rotation, 2° apart), a linear filter sees a
358° step and blends through the middle of it, producing intermediate values
that are genuinely different rotations — for a few frames, until the filter
settles. That would explain the burst length, why it clusters on the one joint
whose rotation lives near 180°, and why smoothing does not remove it.

Note the same function already treats the global rotation correctly, with
`filtfilt_quat(grot, …)`. The per-joint rotations do not get that treatment.

**Cheap check:** print `joint_params_[1*7 + 3 .. 5]` for the pelvis across the
first ~50 frames of `summerlove.mp4`, before and after `smoothing_pass`. If the
raw angles wrap and the smoothed ones ramp through the wrap, that is the bug.
If the raw angles themselves jump ~100°, it is upstream of smoothing, in the
decode or the network output, and the fix belongs there instead.

---

## One thing I could not explain

`videos/summerlove_rendered.mp4` looks **clean** at the corresponding frames.

I aligned the two by cross-correlating per-frame motion energy, which puts the
lag at 119–120 frames — consistent with the frame counts (671 video frames vs
551 tracked). I inspected the render at both candidate alignments and saw a
normally upright dancer every time.

So either the render and the committed `.arfz` came from different invocations
(different flags, or a build from either side of a change), or the render path
does something to the pelvis rotation that the export path does not. Worth
knowing which, since it may already contain the fix.

---

## Suggested directions

1. **Filter rotations as rotations.** Extend the `filtfilt_quat` treatment
   already applied to `grot` to the per-joint rotations, or unwrap the Euler
   channels before `filter_channels` touches them.
2. **Failing that, despike after the FK**, once, somewhere both exporters see —
   rather than only in `gmr_retarget.py`, downstream of the files. Every
   consumer of a `.bvh` or `.arfz` currently meets these frames, and only the
   GMR path is protected.
3. **Consider flagging rather than hiding.** A per-frame confidence or validity
   bit in the exported stream would let a reader decide. ARF has a natural
   place for this: `AAU_BLENDSHAPE` already carries a `has_confidence` byte
   (always 0 today), and an equivalent on `AAU_JOINT` would be
   backward-compatible.

---

## Workaround currently in place downstream

`libarf` implements `arfDespikeFrames()`: it flags frames whose largest
per-joint angular velocity exceeds a threshold (40°/frame, matching
`--despike-root-deg`), rebuilds them by slerp/lerp from the nearest clean
frames either side, then refines the flagged set so that real motion caught
between two nearby bursts is given back rather than interpolated away.

On the sample it rewrites 16 frames and leaves the other 97% untouched. It is
opt-in — a reader that silently rewrote the animation it was asked to read
would be worse than one that shows the glitch — and on by default only in the
interactive viewer.

That is a patch over the symptom, in one reader. The data is still wrong in
every exported file.

---

*Measurements in this report are reproducible with `libarf` and the files
already committed to both repositories; the BVH table needs only Python and
`gmr_out/`.*
