"""Check the zero-wait retry guard against the actual selected Arduino source."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from usb_cdc_patch import MARKER, patch

source = Path(sys.argv[1]).read_text(encoding='utf-8')
# Remove only our marked insertion to exercise both fresh and idempotent paths.
begin = source.find('        ' + MARKER)
if begin >= 0:
    end = source.index('\n        }', begin) + len('\n        }\n')
    source = source[:begin] + source[end:]
updated = patch(source)
assert updated.count(MARKER) == 1
assert patch(updated) == updated
start = updated.index(MARKER)
assert 'if (tx_timeout_ms == 0)' in updated[start:start+400]
assert 'size = so_far;' in updated[start:start+400]
assert 'break;' in updated[start:start+400]
try:
    patch(source.replace('tx_timeout_ms / portTICK_PERIOD_MS', 'unexpected_timeout'))
except RuntimeError:
    pass
else:
    raise AssertionError('Unknown send guard accepted')
print('PASS actual HWCDC zero-wait insertion, idempotence and unknown-source rejection')
