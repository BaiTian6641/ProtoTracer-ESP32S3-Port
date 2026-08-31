# Extended morph analysis: cutoff / dedup / quantization feasibility.
import json, os

def analyze(path):
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    morphs = doc.get("morphs", [])
    print(f"\n=== {path} ===")

    entries = 0
    fully_tiny = 0     # all 3 components |d| < 0.002 (entry droppable)
    fully_zero = 0     # all 3 components == 0
    global_min_abs = float('inf')
    global_max_abs = 0.0
    over_half_range = 0  # |component| > 65504 (half float max) -> none expected
    # for fixed-point feasibility: component magnitude distribution
    mag_buckets = {"<0.002":0, "<0.01":0, "<0.1":0, "<0.5":0, "<1":0, "<4":0, ">=4":0}

    total_delta_vecs = 0
    unique_delta_vecs = set()
    dup_savings_entries = 0

    for m in morphs:
        idxs = m.get("indices", [])
        vecs = m.get("vectors", [])
        n = m.get("vertexCount", len(idxs))
        # dedup within this morph
        seen = {}
        for i in range(n):
            dx = vecs[3*i]; dy = vecs[3*i+1]; dz = vecs[3*i+2]
            entries += 1
            ax, ay, az = abs(dx), abs(dy), abs(dz)
            mx = max(ax, ay, az)
            if dx == 0 and dy == 0 and dz == 0:
                fully_zero += 1
            if mx < 0.002:
                fully_tiny += 1
            if mx > 0:  # nonzero magnitude
                if mx < global_min_abs: global_min_abs = mx
                if mx > global_max_abs: global_max_abs = mx
            for c in (ax, ay, az):
                if c > 65504: over_half_range += 1
                if c < 0.002: mag_buckets["<0.002"] += 1
                elif c < 0.01: mag_buckets["<0.01"] += 1
                elif c < 0.1: mag_buckets["<0.1"] += 1
                elif c < 0.5: mag_buckets["<0.5"] += 1
                elif c < 1: mag_buckets["<1"] += 1
                elif c < 4: mag_buckets["<4"] += 1
                else: mag_buckets[">=4"] += 1
            key = (round(dx,6), round(dy,6), round(dz,6))
            total_delta_vecs += 1
            unique_delta_vecs.add(key)
            if key in seen:
                dup_savings_entries += 1
            seen[key] = seen.get(key, 0) + 1

    print(f"morph entries={entries}")
    print(f"fully-zero entries (delta==0,0,0)         : {fully_zero}  ({100.0*fully_zero/entries:.1f}%) -> droppable")
    print(f"fully-tiny entries (max|d|<0.002)         : {fully_tiny}  ({100.0*fully_tiny/entries:.1f}%) -> droppable")
    print(f"nonzero component magnitude range         : [{global_min_abs:.5f} .. {global_max_abs:.3f}]")
    print(f"components exceeding half-float max (65504): {over_half_range}")
    print(f"component magnitude buckets               : {mag_buckets}")
    print(f"delta vectors: total={total_delta_vecs} unique={len(unique_delta_vecs)} dup-entries={dup_savings_entries} ({100.0*dup_savings_entries/max(1,total_delta_vecs):.1f}% share a delta with another vertex)")

    # Entry-byte model comparison (index + delta), float32 baseline = 16 B
    def kb(b): return b/1024.0
    n = entries
    print(f"\nstorage models for {entries} entries:")
    print(f"  current int32 idx + 3x float32 delta      : 16 B/entry = {n*16} B ({kb(n*16):.1f} KiB)")
    print(f"  uint16 idx + 3x float16 delta             :  8 B/entry = {n*8} B ({kb(n*8):.1f} KiB)")
    print(f"  uint16 idx + 3x int16 fixed (Q, scale)    :  8 B/entry = {n*8} B ({kb(n*8):.1f} KiB)")
    print(f"  uint16 idx + 3x int8  fixed             :  5 B/entry = {n*5} B ({kb(n*5):.1f} KiB)")
    print(f"  dedup delta + uint16 idx (delta table)    : ~{kb(len(unique_delta_vecs)*6 + n*2):.1f} KiB  (delta table {len(unique_delta_vecs)}x6B + idx+ref {n}x(2+2)B)")

for p in ["src/Morph/universal_face.json", "data/universal_face.json"]:
    if os.path.exists(p):
        analyze(p)
