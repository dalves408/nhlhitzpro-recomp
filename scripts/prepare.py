"""Reproduce NHL Hitz Pro analysis and static C translation (Python + Capstone)."""
from pathlib import Path
import argparse
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/xboxrecomp'
OUT = ROOT / 'analysis'
XBE = str(ROOT / 'game_files/default.xbe')
EXTRA = 'XMV,D3D,D3DX,XGRPH,DSOUND,WMADEC,XPP,DOLBY'
D = OUT / 'disasm'
I = OUT / 'identified'
A = OUT / 'abi'
STEPS = [
    ('parse', ['tools.xbe_parser', XBE, '--json', str(ROOT / 'game_files/default_analysis.json')]),
    ('disasm', ['tools.disasm', XBE, '--text-only', '--extra-sections', EXTRA, '--output', str(D), '--verbose']),
    ('identify', ['tools.func_id', XBE, '--functions', str(D / 'functions.json'), '--strings', str(D / 'strings.json'), '--xrefs', str(D / 'xrefs.json'), '--output', str(I)]),
    ('abi', ['tools.abi_analysis', XBE, '--disasm-dir', str(D), '--func-id-dir', str(I), '--output-dir', str(A), '-v']),
    ('recompile', ['tools.recomp', XBE, '--all', '--split', '250', '--game-name', 'NHL Hitz Pro', '--disasm-dir', str(D), '--func-id-dir', str(I), '--abi-dir', str(A), '--gen-dir', str(ROOT / 'native/src/recomp/gen'), '--output-dir', str(OUT / 'recomp')]),
]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--from-stage', choices=[n for n, _ in STEPS], default='parse')
    ap.add_argument('--full', action='store_true', help='Compatibility flag; a full pipeline is the default')
    ap.add_argument('--split-asm', action='store_true', help='Also export one assembly file per function (runs discovery again)')
    args = ap.parse_args()
    import capstone
    print(f'Python {sys.version.split()[0]}, Capstone {capstone.__version__}', flush=True)
    OUT.mkdir(exist_ok=True)
    stages = STEPS[[n for n, _ in STEPS].index(args.from_stage):]
    if args.split_asm:
        stages = stages + [('split', ['tools.split', XBE, '--text-only', '--extra-sections', EXTRA, '--abi', str(A / 'abi_functions.json'), '--out', str(OUT / 'split'), '-v'])]
    for name, argv in stages:
        logpath = OUT / f'{name}.log'
        print(f'{name}: {logpath}', flush=True)
        with logpath.open('w', encoding='utf-8') as log:
            result = subprocess.run([sys.executable, '-u', str(ROOT / 'scripts/tool_runner.py'), *argv], cwd=TOOL, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            print(logpath.read_text(encoding='utf-8', errors='replace')[-6000:], file=sys.stderr)
            return result.returncode
        print(f'{name}: complete', flush=True)
    print('Requested preparation stages complete. Generated C still requires build and runtime validation.', flush=True)
    return 0

if __name__ == '__main__':
    sys.exit(main())
