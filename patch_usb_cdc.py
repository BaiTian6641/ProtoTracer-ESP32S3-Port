"""Reproducible bounded-write patch for the selected S3 Arduino framework."""
import importlib.util
from pathlib import Path
from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()
if env.BoardConfig().get('build.mcu') == 'esp32s3':
    project = Path(env.subst('$PROJECT_DIR'))
    spec = importlib.util.spec_from_file_location('usb_cdc_patch', project / 'tools/usb_cdc_patch.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    framework = Path(env.PioPlatform().get_package_dir('framework-arduinoespressif32'))
    source = framework / 'cores/esp32/HWCDC.cpp'
    original = source.read_bytes()
    text = original.decode('utf-8').replace('\r\n', '\n')
    updated = module.patch(text)
    if updated != text:
        newline = '\r\n' if b'\r\n' in original else '\n'
        source.write_bytes(updated.replace('\n', newline).encode('utf-8'))
        print('[USB] Patched HWCDC zero-timeout retry guard')
