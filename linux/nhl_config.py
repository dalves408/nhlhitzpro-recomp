#!/usr/bin/env python3
"""NHL Hitz Pro - Settings (Linux port of native/config/config.c).
Reads and writes the same settings.ini the Linux game uses:
$XDG_DATA_HOME/nhlhitzpro/settings.ini (default ~/.local/share/nhlhitzpro/settings.ini),
sections [display] and [textures], and keymap.ini beside it ([keys], Windows virtual-key codes,
the same file config.exe writes; overrides/keyboard.c reads it). play.sh leaves these to the game,
which reads them at start-up."""
import os, configparser, tkinter as tk
from tkinter import ttk

def ini_path(name="settings.ini"):
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "nhlhitzpro", name)

# Controls: native/overrides/nhl_keymap.h (id, label, key, alternate key), Windows VK codes, 0 = none.
KEYMAP = [
    ("dpad_up", "D-pad up", 0x26, 0), ("dpad_down", "D-pad down", 0x28, 0),
    ("dpad_left", "D-pad left", 0x25, 0), ("dpad_right", "D-pad right", 0x27, 0),
    ("start", "Start", 0x0D, 0), ("back", "Back", 0x08, 0),
    ("lthumb", "Left stick click", 0x10, 0), ("rthumb", "Right stick click", 0x11, 0),
    ("a", "A", ord("Z"), 0), ("b", "B", ord("X"), 0), ("x", "X", ord("C"), 0), ("y", "Y", ord("V"), 0),
    ("white", "White", ord("Q"), 0), ("black", "Black", ord("E"), 0),
    ("ltrigger", "Left trigger", ord("1"), 0), ("rtrigger", "Right trigger", ord("3"), 0),
    ("lstick_up", "Left stick up", ord("W"), 0x68), ("lstick_down", "Left stick down", ord("S"), 0x62),
    ("lstick_left", "Left stick left", ord("A"), 0x64), ("lstick_right", "Left stick right", ord("D"), 0x66),
    ("rstick_up", "Right stick up", ord("I"), 0), ("rstick_down", "Right stick down", ord("K"), 0),
    ("rstick_left", "Right stick left", ord("J"), 0), ("rstick_right", "Right stick right", ord("L"), 0),
]
# Tk keysym -> the VK the game sees for that key. Mirrors xbox_InputSdlKeyToVk (keyboard_sdl.c):
# SDL keycodes follow the layout like Tk keysyms do, and Shift/Ctrl/Alt are one VK for either side.
_VK = {"Return": 0x0D, "KP_Enter": 0x0D, "BackSpace": 0x08, "Shift_L": 0x10, "Shift_R": 0x10,
       "Control_L": 0x11, "Control_R": 0x11, "Alt_L": 0x12, "Alt_R": 0x12, "ISO_Level3_Shift": 0x12,
       "Meta_L": 0x12, "Meta_R": 0x12, "Super_L": 0x5B, "Super_R": 0x5C,
       "Up": 0x26, "Down": 0x28, "Left": 0x25, "Right": 0x27, "Tab": 0x09, "ISO_Left_Tab": 0x09,
       "Escape": 0x1B, "space": 0x20, "Pause": 0x13, "Caps_Lock": 0x14, "Prior": 0x21, "Next": 0x22,
       "End": 0x23, "Home": 0x24, "Insert": 0x2D, "Delete": 0x2E,
       "KP_Multiply": 0x6A, "KP_Add": 0x6B, "KP_Subtract": 0x6D, "KP_Decimal": 0x6E, "KP_Delete": 0x6E,
       "KP_Separator": 0x6E, "KP_Divide": 0x6F,
       "semicolon": 0xBA, "colon": 0xBA, "equal": 0xBB, "plus": 0xBB, "comma": 0xBC, "less": 0xBC,
       "minus": 0xBD, "underscore": 0xBD, "period": 0xBE, "greater": 0xBE, "slash": 0xBF, "question": 0xBF,
       "grave": 0xC0, "asciitilde": 0xC0, "bracketleft": 0xDB, "braceleft": 0xDB, "backslash": 0xDC, "bar": 0xDC,
       "bracketright": 0xDD, "braceright": 0xDD, "apostrophe": 0xDE, "quotedbl": 0xDE,
       "exclam": 0x31, "at": 0x32, "numbersign": 0x33, "dollar": 0x34, "percent": 0x35,
       "asciicircum": 0x36, "ampersand": 0x37, "asterisk": 0x38, "parenleft": 0x39, "parenright": 0x30}
# SDL gives the keypad's digit keycodes whatever Num Lock says; Tk gives the navigation names when it is off.
for _i, _n in enumerate(["Insert", "End", "Down", "Next", "Left", "Begin", "Right", "Home", "Up", "Prior"]):
    _VK["KP_%d" % _i] = 0x60 + _i; _VK["KP_" + _n] = 0x60 + _i
for _i in range(1, 13): _VK["F%d" % _i] = 0x6F + _i

def keysym_vk(sym):
    if len(sym) == 1 and sym.isascii() and sym.isalnum(): return ord(sym.upper())
    return _VK.get(sym, 0)

_NAMES = {0x0D: "Enter", 0x08: "Backspace", 0x10: "Shift", 0x11: "Ctrl", 0x12: "Alt", 0x26: "Up", 0x28: "Down",
          0x25: "Left", 0x27: "Right", 0x09: "Tab", 0x1B: "Esc", 0x20: "Space", 0x13: "Pause", 0x14: "Caps Lock",
          0x21: "Page Up", 0x22: "Page Down", 0x23: "End", 0x24: "Home", 0x2D: "Insert", 0x2E: "Delete",
          0x6A: "Num *", 0x6B: "Num +", 0x6D: "Num -", 0x6E: "Num Del", 0x6F: "Num /", 0xBA: ";", 0xBB: "=",
          0xBC: ",", 0xBD: "-", 0xBE: ".", 0xBF: "/", 0xC0: "`", 0xDB: "[", 0xDC: "\\", 0xDD: "]", 0xDE: "'",
          0x5B: "Left Windows", 0x5C: "Right Windows"}

def key_name(vk):
    if not vk: return "(none)"
    if vk in _NAMES: return _NAMES[vk]
    if 0x30 <= vk <= 0x39 or 0x41 <= vk <= 0x5A: return chr(vk)
    if 0x60 <= vk <= 0x69: return "Num %d" % (vk - 0x60)
    if 0x70 <= vk <= 0x7B: return "F%d" % (vk - 0x6F)
    return "Key %d" % vk

T_RES = ["1x  640x480 (Native)", "2x  1280x960", "3x  1920x1440", "4x  2560x1920", "5x  3200x2400"]
T_OFFON = ["Off", "On"]
T_STRETCH = ["Off (4:3)", "On (fills window)"]
T_FPS = ["30", "60 (original)", "120", "144", "240", "Uncapped"]
T_AA = ["Off", "2x MSAA", "4x MSAA", "8x MSAA"]
T_ANISO = ["Off", "2x", "4x", "8x", "16x"]
V_RES = [1, 2, 3, 4, 5]; V_OFFON = [0, 1]; V_FPS = [30, 60, 120, 144, 240, 0]; V_AA = [1, 2, 4, 8]; V_ANISO = [1, 2, 4, 8, 16]
# label, key, texts, values, default index, available on Linux
VIDEO = [
    ("Render Scale", "resolution_scale", T_RES, V_RES, 0, True),
    ("Fullscreen", "fullscreen", T_OFFON, V_OFFON, 1, True),
    ("Stretch image", "stretch", T_STRETCH, V_OFFON, 0, True),
    ("VSync", "vsync", T_OFFON, V_OFFON, 0, True),
    ("Frame Rate", "frame_rate", T_FPS, V_FPS, 1, True),
    ("Anti-aliasing", "msaa", T_AA, V_AA, 0, True),
    ("Anisotropic filter", "anisotropy", T_ANISO, V_ANISO, 0, True),
]
TEX = [("enabled", "Use hi-res textures (Render Scale 2x or higher)", 0), ("rink", "Rink", 1), ("logos", "Logos", 1),
       ("jerseys", "Jerseys", 1), ("glow", "Glow", 1), ("other", "Other", 1)]

def nearest(values, v):
    if v is None: return None
    return min(range(len(values)), key=lambda i: abs(values[i] - v))

class App:
    def __init__(self, root):
        self.root = root
        root.title("NHL Hitz Pro - Settings"); root.geometry("600x620"); root.resizable(False, False)
        # Prefer the Pro icon shipped beside the settings app / install folder.
        here = os.path.dirname(os.path.abspath(__file__))
        for candidate in (
            os.path.join(here, "icon.png"),
            os.path.join(here, "icons", "nhlhitzpro-64.png"),
            os.path.join(here, "..", "native", "resources", "nhl_hitz_pro.png"),
            os.path.expanduser("~/.local/share/icons/hicolor/64x64/apps/nhlhitzpro.png"),
        ):
            if os.path.isfile(candidate):
                try:
                    self._icon = tk.PhotoImage(file=candidate); root.iconphoto(True, self._icon)
                except tk.TclError:
                    continue
                break
        nb = ttk.Notebook(root); nb.place(x=10, y=10, width=580, height=500)
        vf = ttk.Frame(nb); nb.add(vf, text="Video")
        cf = ttk.Frame(nb); nb.add(cf, text="Controls")
        tf = ttk.Frame(nb); nb.add(tf, text="Textures")
        self.nb, self.cf = nb, cf
        ttk.Label(vf, text="Video options. These are the same as the game's F1 menu; set them here before you start the game.",
                  wraplength=540).place(x=20, y=14)
        self.combos = []
        for i, (label, key, texts, values, d, ok) in enumerate(VIDEO):
            y = 64 + i * 40
            ttk.Label(vf, text=label).place(x=30, y=y + 4)
            c = ttk.Combobox(vf, values=texts if ok else ["Not available on Linux"], state="readonly" if ok else "disabled", width=34)
            c.place(x=220, y=y); self.combos.append(c)
        ttk.Label(vf, text="Fullscreen starts the game in a borderless full-screen window. Stretch image fills the window "
                  "instead of keeping the 4:3 shape. Hi-res textures need Render Scale 2x or higher. Changes made in the game's F1 menu apply at once.",
                  wraplength=540).place(x=20, y=64 + len(VIDEO) * 40 + 6)
        ttk.Label(tf, text="Hi-res texture pack. Each kind can be switched off on its own.", wraplength=540).place(x=20, y=14)
        self.tex = []; self.texcb = []
        for i, (key, label, d) in enumerate(TEX):
            v = tk.IntVar(); cb = ttk.Checkbutton(tf, text=label, variable=v)
            cb.place(x=30 if i == 0 else 50, y=60 + i * 34); self.tex.append(v); self.texcb.append(cb)
        ttk.Button(tf, text="Open texture pack download page…",
                   command=lambda: os.system("xdg-open https://github.com/dalves408/nhlhitzpro-recomp/releases/tag/textures-1")).place(
            x=30, y=60 + len(TEX) * 34 + 12, width=280, height=28)
        ttk.Label(tf, text="Hi-res textures live in the game folder's texture_pack/ (installer option) or can be added later.",
                  wraplength=540).place(x=20, y=60 + len(TEX) * 34 + 48)
        self.combos[0].bind("<<ComboboxSelected>>", lambda e: self.scale_changed(), add="+")
        # Controls tab, as config.exe's: the list, then Change key / Change alternate / Clear.
        self.keys = [[k0, k1] for (i, l, k0, k1) in KEYMAP]; self.wait = None
        tv = ttk.Treeview(cf, columns=("key", "alt"), show="tree headings", selectmode="browse", height=18)
        tv.heading("#0", text="Action", anchor="w"); tv.heading("key", text="Key", anchor="w")
        tv.heading("alt", text="Alternate key", anchor="w")
        tv.column("#0", width=210, stretch=False); tv.column("key", width=165, stretch=False); tv.column("alt", width=165, stretch=False)
        for n, (i, label, k0, k1) in enumerate(KEYMAP): tv.insert("", "end", iid=str(n), text=label)
        tv.place(x=10, y=10, width=556, height=400); tv.selection_set("0"); tv.focus("0")
        tv.bind("<Double-1>", lambda e: self.begin_wait(0)); self.tv = tv
        ttk.Button(cf, text="Change key", command=lambda: self.begin_wait(0)).place(x=10, y=420, width=100, height=28)
        ttk.Button(cf, text="Change alternate", command=lambda: self.begin_wait(1)).place(x=116, y=420, width=130, height=28)
        ttk.Button(cf, text="Clear", command=self.clear_key).place(x=252, y=420, width=70, height=28)
        root.bind_all("<KeyPress>", self.on_key, add="+")
        nb.bind("<<NotebookTabChanged>>", self.tab_changed)
        self.status = ttk.Label(root, text="", wraplength=580); self.status.place(x=10, y=514)
        ttk.Button(root, text="Reset to defaults", command=self.reset).place(x=10, y=548, width=130, height=28)
        ttk.Button(root, text="Save", command=self.save).place(x=444, y=548, width=70, height=28)
        ttk.Button(root, text="Close", command=root.destroy).place(x=520, y=548, width=70, height=28)
        self.load()

    def load(self):
        cp = configparser.ConfigParser(); cp.optionxform = str; cp.read(ini_path())
        for c, (label, key, texts, values, d, ok) in zip(self.combos, VIDEO):
            if not ok: c.set("Not available on Linux"); continue
            try: idx = nearest(values, cp.getint("display", key))
            except Exception: idx = d
            if key == "frame_rate" and cp.has_option("display", key) and cp.getint("display", key) >= 360: idx = 5
            c.current(idx)
        for v, (key, label, d) in zip(self.tex, TEX):
            try: v.set(1 if cp.getint("textures", key) else 0)
            except Exception: v.set(d)
        km = configparser.ConfigParser(); km.optionxform = str
        try: km.read(ini_path("keymap.ini"))
        except configparser.Error: pass
        for n, (i, label, k0, k1) in enumerate(KEYMAP):
            for s, (opt, d) in enumerate(((i, k0), (i + "_alt", k1))):
                try: v = km.getint("keys", opt)
                except Exception: v = -1
                self.keys[n][s] = v if 0 <= v < 256 else d
        self.refresh_all(); self.scale_changed()
        self.status.config(text="Settings file: " + ini_path())

    def scale_changed(self):
        # item 27: at Render Scale 1x hi-res is off and can't be switched on, as in the F1 menu
        low = self.combos[0].current() == 0
        if low: self.tex[0].set(0)
        self.texcb[0].config(text="Use hi-res textures: Off (Needs 2x+)" if low else TEX[0][1])
        for cb in self.texcb: cb.state(["disabled"] if low else ["!disabled"])

    # ---- Controls ----
    def refresh_row(self, n):
        self.tv.item(str(n), values=(key_name(self.keys[n][0]), key_name(self.keys[n][1])))

    def refresh_all(self):
        for n in range(len(KEYMAP)): self.refresh_row(n)

    def row(self):
        sel = self.tv.selection()
        return int(sel[0]) if sel else None

    def tab_changed(self, e=None):
        self.wait = None
        t = self.nb.index("current")
        self.status.config(text=["Video: these are also in the game's F1 menu, where changes apply at once.",
                                 "Controls: select an action, click Change key (or double-click it), then press the key you want.",
                                 "Textures: hi-res textures need Render Scale 2x or higher (Video tab)."][t])

    def begin_wait(self, slot):
        n = self.row()
        if n is None: self.status.config(text="Select an action in the list first."); return
        self.wait = (n, slot)
        self.status.config(text='Press the new %s for "%s"  (Esc cancels).' % ("alternate key" if slot else "key", KEYMAP[n][1]))
        self.root.focus_set()   # the key goes to the window, not a button

    def on_key(self, e):
        if self.wait is None: return
        if e.keysym == "Escape": self.wait = None; self.status.config(text="Cancelled."); return "break"
        vk = keysym_vk(e.keysym)
        if not vk: self.status.config(text="The game can't read that key. Press another key."); return "break"
        if vk in (0x70, 0x77, 0x7A, 0x12, 0x5B, 0x5C):
            self.status.config(text="%s is used by the game itself (menus, textures, fullscreen, Alt+Enter, Esc) and can't be "
                               "assigned. Press another key." % key_name(vk)); return "break"
        n, slot = self.wait; moved = None
        for i in range(len(KEYMAP)):   # a key drives one action: take it from the old one
            for s in range(2):
                if self.keys[i][s] == vk and (i, s) != (n, slot):
                    self.keys[i][s] = 0; self.refresh_row(i); moved = KEYMAP[i][1]
        self.keys[n][slot] = vk; self.refresh_row(n); self.wait = None
        self.status.config(text='%s was moved here from "%s".' % (key_name(vk), moved) if moved
                           else '"%s" is now %s.' % (KEYMAP[n][1], key_name(vk)))
        return "break"

    def clear_key(self):
        n = self.row()
        if n is None: self.status.config(text="Select an action in the list first."); return
        self.keys[n] = [0, 0]; self.refresh_row(n)
        self.status.config(text="Cleared. That action has no key until you assign one.")

    def reset(self):
        t = self.nb.index("current"); self.wait = None
        if t == 0:
            for c, (label, key, texts, values, d, ok) in zip(self.combos, VIDEO):
                if ok: c.current(d)
            self.scale_changed(); what = "video settings"
        elif t == 1:
            self.keys = [[k0, k1] for (i, l, k0, k1) in KEYMAP]; self.refresh_all(); what = "keys"
        else:
            for v, (key, label, d) in zip(self.tex, TEX): v.set(d)
            self.scale_changed(); what = "texture settings"
        self.status.config(text="Default %s restored. Press Save to keep them." % what)

    def save(self):
        p = ini_path(); os.makedirs(os.path.dirname(p), exist_ok=True)
        cp = configparser.ConfigParser(); cp.optionxform = str; cp.read(p)
        for s in ("display", "textures"):
            if not cp.has_section(s): cp.add_section(s)
        for c, (label, key, texts, values, d, ok) in zip(self.combos, VIDEO):
            if ok: cp.set("display", key, str(values[c.current()]))
        for v, (key, label, d) in zip(self.tex, TEX): cp.set("textures", key, str(v.get()))
        tmp = p + ".tmp"
        with open(tmp, "w") as f: cp.write(f, space_around_delimiters=False)
        os.replace(tmp, p)
        kp = ini_path("keymap.ini")
        km = configparser.ConfigParser(); km.optionxform = str
        try: km.read(kp)
        except configparser.Error: km = configparser.ConfigParser(); km.optionxform = str
        if not km.has_section("keys"): km.add_section("keys")
        for n, (i, label, k0, k1) in enumerate(KEYMAP):
            km.set("keys", i, str(self.keys[n][0])); km.set("keys", i + "_alt", str(self.keys[n][1]))
        with open(kp + ".tmp", "w") as f: km.write(f, space_around_delimiters=False)
        os.replace(kp + ".tmp", kp)
        self.status.config(text="Saved. The game uses these the next time it starts.")

if __name__ == "__main__":
    root = tk.Tk(); App(root); root.mainloop()
