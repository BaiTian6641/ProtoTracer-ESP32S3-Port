"""Make Arduino HWCDC zero-timeout writes return instead of retrying forever."""
import re

MARKER = '// PROTOTRACER_HWCDC_ZERO_WAIT_V1'


def patch(source):
    start = source.index('size_t HWCDC::write(const uint8_t *buffer, size_t size)')
    end = source.index('size_t HWCDC::write(uint8_t c)', start)
    body = source[start:end]
    if MARKER in body:
        return source
    if 'so_far' not in body or 'tx_timeout_ms' not in body:
        raise RuntimeError('Unsupported HWCDC write implementation')
    pattern = r'(if \(xRingbufferSend\(tx_ring_buf, [^\n]*tx_timeout_ms / portTICK_PERIOD_MS\) != pdTRUE\) \{\n)'
    matches = list(re.finditer(pattern, body))
    if len(matches) != 1:
        raise RuntimeError('HWCDC blocking-send guard changed; review the patch')
    insertion = matches[0].end()
    guard = (
        '        ' + MARKER + '\n'
        '        // A plugged-in host may never open/read CDC. Zero timeout must\n'
        '        // also bound retries, including while USB SOF remains active.\n'
        '        if (tx_timeout_ms == 0) {\n'
        '          size = so_far;\n'
        '          break;\n'
        '        }\n'
    )
    body = body[:insertion] + guard + body[insertion:]
    return source[:start] + body + source[end:]
