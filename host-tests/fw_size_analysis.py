# Robust GNU ld map parser: output-section header at column 0 sets context;
# an input entry is "<secname>" on one line then "0xADDR 0xSIZE <object>" on the
# next. Aggregate flashed bytes per object file, rolled up to library.
import re, sys
from collections import defaultdict

MAP = sys.argv[1] if len(sys.argv) > 1 else r".pio\build\esp32s3-RELEASE\firmware.map"

# column-0 output section header: ".flash.text   0x42000020   0x142f94"
out_hdr = re.compile(r'^(\.\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s*$')
# input entry addr+size+object: "   0x42000020       0x1c path/to/obj.o"
entry = re.compile(r'^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S.*)$')

def libname(path):
    p = path.replace('\\', '/')
    if '.pio/libdeps/' in p:
        sub = p.split('.pio/libdeps/')[1].split('/')
        return f"lib:{sub[1] if len(sub)>1 else sub[0]}"
    if 'framework-arduinoespressif32/' in p and '-libs' not in p:
        if '/cores/' in p: return "arduino-core"
        if '/libraries/' in p: return "arduino-lib:" + p.split('/libraries/')[1].split('/')[0]
        return "arduino-framework"
    if 'framework-arduinoespressif32-libs/' in p:
        # the prebuilt ESP-IDF libs (WiFi/BT/lwip/etc.) live under .../esp32s3/lib or .../ld
        m = re.search(r'/esp32s3/(?:lib|ld)/(lib[^/]+\.a)', p)
        if m: return "esp-idf:" + m.group(1)
        return "esp-idf-libs"
    if 'toolchain' in p or 'xtensa-esp' in p:
        m = re.search(r'(libstdc\+\+\.a|libc\.a|libgcc\.a|libm\.a|libstdc.*\.a)', p)
        return "toolchain:" + (m.group(1) if m else "other")
    if '/src/' in p:
        s = p.split('/src/')[-1]
        return "firmware-src/" + (s.split('/')[0] if '/' in s else s)
    if '/lib/' in p:
        return "vendored:" + p.split('/lib/')[-1].split('/')[0]
    if '/boards/' in p or p.endswith('.S.obj'):
        return "asm/board"
    return "other:" + p.split('/')[-1]

per_obj = defaultdict(int)
per_lib = defaultdict(int)
cur = None
lib_of_obj = {}
with open(MAP, 'r', encoding='utf-8', errors='replace') as f:
    for line in f:
        h = out_hdr.match(line)
        if h:
            cur = h.group(1)
            continue
        e = entry.match(line)
        if not e or cur is None:
            continue
        size = int(e.group(2), 16)
        obj = e.group(3).strip()
        if size == 0 or obj.startswith('('):   # skip "(size before relaxing)"
            continue
        # only count sections that occupy flash or ram
        if not (cur.startswith('.flash') or cur.startswith('.iram') or cur.startswith('.dram') or cur.startswith('.ext_ram')):
            continue
        per_obj[obj] += size
        lib = libname(obj)
        lib_of_obj[obj] = lib
        per_lib[lib] += size

total = sum(per_lib.values())
print(f"=== attributed flash/iram/dram total: {total} bytes ({total/1024:.1f} KiB / {total/1048576:.2f} MB) ===\n")
print("=== TOP 30 modules/libs by flashed bytes ===")
for lib, sz in sorted(per_lib.items(), key=lambda kv: -kv[1])[:30]:
    print(f"  {sz:>9} B ({sz/1024:>8.1f} KiB)  {100.0*sz/total:5.1f}%  {lib}")

print("\n=== TOP 20 object files by flashed bytes ===")
for obj, sz in sorted(per_obj.items(), key=lambda kv: -kv[1])[:20]:
    short = obj.replace('\\', '/')
    if len(short) > 95: short = '...' + short[-92:]
    print(f"  {sz:>9} B ({sz/1024:>8.1f} KiB)  [{lib_of_obj.get(obj,'?')}]  {short}")
