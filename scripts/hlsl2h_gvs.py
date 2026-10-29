"""Embed native/src/d3d11_gvs.hlsl as a C string: native/src/d3d11_gvs_hlsl.h.
Run: python scripts/hlsl2h_gvs.py  (from the repo root)"""
src = open('native/src/d3d11_gvs.hlsl', 'rb').read().decode('utf-8').replace('\r\n', '\n')
out = ['/* Generated from d3d11_gvs.hlsl by scripts/hlsl2h_gvs.py: do not edit. */',
       'static const char k_hlsl_gvs[] =']
for line in src.rstrip('\n').split('\n'):
    out.append('    "' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"')
out[-1] += ';'
open('native/src/d3d11_gvs_hlsl.h', 'wb').write(('\r\n'.join(out) + '\r\n').encode('utf-8'))