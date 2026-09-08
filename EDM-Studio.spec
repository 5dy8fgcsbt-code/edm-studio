# -*- mode: python ; coding: utf-8 -*-
import os
import sys
from pathlib import Path

# Qt uses the Windows ICU API. Build-host tools such as Poppler can put an
# incompatible icuuc.dll on PATH; do not let their DLLs enter the application.
os.environ['PATH'] = os.pathsep.join([
    str(Path(sys.executable).parent), str(Path(sys.base_prefix)),
    str(Path(os.environ['SystemRoot']) / 'System32'), os.environ['SystemRoot'],
])


a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=[],
    datas=[('edm_studio/edm/LICENSE', 'licenses/edm-parser')],
    hiddenimports=['OpenGL.platform.win32','lupa.lua54'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='EDM-Studio',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='EDM-Studio',
)
