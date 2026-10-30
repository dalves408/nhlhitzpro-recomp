"""Turn a [KEYLOG] recording (RecordMenu.bat / Playtest.bat) into a menu replay file.

usage: python scripts/keylog_to_menu.py <recorded log> <out file>

The replay file has one press per line:  <seconds since the previous press> <button> <hold ms>
The leading Start presses (intro movies and the title screen) are dropped: quick_match.ps1 handles those by
watching the game, then replays this file from the first press that is not Start.
"""
import re, sys

def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    ev = []   # (t_down, t_up, button)
    down = {}
    for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
        m = re.search(r'\[KEYLOG\] t=([0-9.]+) (down|up)\s+vk=0x[0-9A-Fa-f]+ pad=(\S+)', line)
        if not m:
            continue
        t, kind, btn = float(m.group(1)), m.group(2), m.group(3)
        if kind == 'down':
            down[btn] = t
        elif btn in down:
            ev.append((down.pop(btn), t, btn))
    ev.sort()
    while ev and ev[0][2] == 'start':          # intro and title Start presses
        ev.pop(0)
    if not ev:
        raise SystemExit('no menu presses found after the Start presses')
    out, prev = [], None
    for td, tu, btn in ev:
        if prev is not None and td - prev > 30:   # a long pause: the match started, the rest is gameplay
            break
        gap = 2.0 if prev is None else max(0.05, td - prev)
        hold = max(100, int(round((tu - td) * 1000)))
        out.append(f'{gap:.2f} {btn} {hold}')
        prev = td
    open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
    print(f'{len(out)} presses, {sum(float(l.split()[0]) for l in out):.1f} s -> {sys.argv[2]}')

if __name__ == '__main__':
    main()
