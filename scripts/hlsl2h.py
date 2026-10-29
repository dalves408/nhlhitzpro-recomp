"""Embed native/src/d3d11_rc.hlsl as a C string: native/src/d3d11_rc_hlsl.h.
Run: python scripts/hlsl2h.py  (from the repo root)"""
src = open('native/src/d3d11_rc.hlsl', 'rb').read().decode('utf-8').replace('\r\n', '\n')
out = ['/* Generated from d3d11_rc.hlsl by scripts/hlsl2h.py: do not edit. */',
       'static const char k_hlsl_rc[] =']
for line in src.split('\n'):
    out.append('    "' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"')
out[-1] += ';'
open('native/src/d3d11_rc_hlsl.h', 'wb').write(('\r\n'.join(out) + '\r\n').encode('utf-8'))