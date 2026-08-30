# Face-model memory analysis for ProtoTracer morph optimization planning.
# Run with the PlatformIO penv python (no extra deps).
import json, sys, os

def analyze(path, used_names):
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    vc = doc.get("vertexCount"); tc = doc.get("triangleCount"); mc = doc.get("morphCount")
    morphs = doc.get("morphs", [])
    print(f"\n=== {path} ===")
    print(f"vertexCount={vc} triangleCount={tc} morphCount={mc} (actual morphs array len={len(morphs)})")
    # base mesh memory (runtime, float32):
    #   vertexBuffer: Vector3D[vc] = vc*12 ; indexBuffer: IndexGroup[tc] = tc*12
    base_mesh = vc*12 + tc*12
    print(f"base mesh runtime bytes (float32): {base_mesh}  (verts {vc*12} + indices {tc*12})")

    total_entries = 0
    tiny_components = 0       # |delta component| < 0.002 and != 0
    zero_components = 0       # exactly 0
    total_components = 0
    morph_rows = []           # (name, vCount, idx_span_min, idx_span_max, span_len, contiguous?, bytes16, bytes_half)
    used_bytes = 0
    unused_bytes = 0
    used_entries = 0
    unused_entries = 0
    used_cnt = 0
    unused_cnt = 0
    unused_names = []
    missing_used = []         # used but not present in face

    face_names = set()
    for m in morphs:
        name = m.get("name", "")
        face_names.add(name)
        idxs = m.get("indices", [])
        vecs = m.get("vectors", [])
        n = m.get("vertexCount", len(idxs))
        total_entries += n
        # value stats
        for v in vecs:
            av = abs(v)
            total_components += 1
            if v == 0:
                zero_components += 1
            elif av < 0.002:
                tiny_components += 1
        # index span / contiguity
        if idxs:
            mn, mx = min(idxs), max(idxs)
            span = mx - mn + 1
            contiguous = (span == len(idxs)) and (sorted(idxs) == list(range(mn, mx+1)))
        else:
            mn = mx = -1; span = 0; contiguous = False
        b16 = n * 16            # int idx (4) + Vector3D (12) as float32
        bh  = n * (2 + 6)       # uint16 idx + 3*half(2)  (packed)
        morph_rows.append((name, n, mn, mx, span, contiguous, b16, bh))
        is_used = name in used_names
        if is_used:
            used_bytes += b16; used_entries += n; used_cnt += 1
        else:
            unused_bytes += b16; unused_entries += n; unused_cnt += 1
            unused_names.append(name)

    for nm in used_names:
        if nm not in face_names:
            missing_used.append(nm)

    total_morph_bytes = total_entries * 16
    print(f"\n-- morph storage (float32: 16 B/entry) --")
    print(f"total morph entries={total_entries}  bytes={total_morph_bytes} ({total_morph_bytes/1024:.1f} KiB)")
    print(f"USED morphs   : count={used_cnt}  entries={used_entries}  bytes={used_bytes} ({used_bytes/1024:.1f} KiB)")
    print(f"UNUSED morphs : count={unused_cnt}  entries={unused_entries}  bytes={unused_bytes} ({unused_bytes/1024:.1f} KiB)")
    print(f"components: total={total_components} zero={zero_components} tiny(|d|<0.002)={tiny_components} -> removable-by-cutoff={(zero_components+tiny_components)} ({100.0*(zero_components+tiny_components)/max(1,total_components):.1f}%)")

    # contiguity
    contig = sum(1 for r in morph_rows if r[5])
    print(f"morphs with contiguous index range: {contig}/{len(morph_rows)}")
    # avg span density for non-contiguous (used for range-pack feasibility)
    sparse = [r for r in morph_rows if not r[5] and r[4] > 0]
    if sparse:
        avg_dens = sum(r[1]/r[4] for r in sparse)/len(sparse)
        print(f"non-contiguous morphs avg index density (vCount/span): {avg_dens:.2f}")

    print(f"\nused-but-missing-in-face (aliases needed?): {missing_used}")
    print(f"UNUSED morph names ({len(unused_names)}):")
    for nm in sorted(unused_names):
        print("   -", nm)
    # biggest morphs by bytes
    morph_rows.sort(key=lambda r: -r[6])
    print(f"\ntop 12 morphs by stored bytes (name, vCount, bytes16, idx[min..max], contiguous):")
    for r in morph_rows[:12]:
        print(f"   {r[0]:24s} vCount={r[1]:4d} bytes={r[6]:6d} idx[{r[2]}..{r[3]}] contig={r[5]}")
    return base_mesh, total_morph_bytes

# Used morph set derived from src/Animation/example_animation.json + JsonDrivenProtogenAnimation.h
used = set([
  # visemes (code): vrc_v_ss/ee/ih/dd/rr/ch/aa/oh
  "vrc_v_ss","vrc_v_ee","vrc_v_ih","vrc_v_dd","vrc_v_rr","vrc_v_ch","vrc_v_aa","vrc_v_oh",
  # blink / mouth (code)
  "Blink","SEyeBlink","HideMouth",
  # auto_link specials
  "HideBlush","HideSecondEye","Zzz","MouthEnd",
  # expression anim_parameter names (example_animation.json)
  "HideEye","EyeNY","Heart","RoundEye","Flat","UwU","XwX","Happy","Anger","Sadness","Frown",
  "Surprised","Doubt","Shy","Question","ALLHIDE","HollowSquareEye","HollowRoundEye","Tones",
  "Exclamation","ZzzMove","ExpressionArrow",
])

base, morph = None, None
for p in ["src/Morph/universal_face.json", "data/universal_face.json"]:
    if os.path.exists(p):
        b, m = analyze(p, used)
        if p.startswith("src/Morph"):
            base, morph = b, m

print("\n=== half-float estimate (on the larger face) ===")
print(f"Vector3D float32 = 12 B/vec ; half float16 = 6 B/vec")
print(f"base mesh: {base} -> {base//2} B (save {base//2} B)")
print(f"morph vectors: each entry Vector3D 12->6 B; idx int->uint16 4->2 B")
