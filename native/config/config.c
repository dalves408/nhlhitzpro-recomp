/* config.exe -- the player's settings program, installed next to the game.
 * For now: a keyboard remapper. Each Xbox control has a key and an alternate key; the
 * choices are saved to %LOCALAPPDATA%\NHLHitzPro\keymap.ini (see overrides/nhl_keymap.h)
 * and the game reads them when it starts. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include "../overrides/nhl_keymap.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const char *const IDS[NHL_KEYMAP_COUNT] = {
#define X(id, label, k0, k1) #id,
    NHL_KEYMAP(X)
#undef X
};
static const char *const LABELS[NHL_KEYMAP_COUNT] = {
#define X(id, label, k0, k1) label,
    NHL_KEYMAP(X)
#undef X
};
static const int DEFAULTS[NHL_KEYMAP_COUNT][2] = {
#define X(id, label, k0, k1) { k0, k1 },
    NHL_KEYMAP(X)
#undef X
};

enum { ID_LIST = 100, ID_CHANGE, ID_ALT, ID_CLEAR, ID_RESET, ID_SAVE, ID_CLOSE, ID_STATUS };

static int s_key[NHL_KEYMAP_COUNT][2];
static int s_dirty;
static int s_wait_row = -1, s_wait_slot;      /* waiting for a key press for this cell */
static HWND s_main, s_list, s_status;

static void key_name(int vk, char *out, size_t n)
{
    LONG sc;
    if (!vk) { snprintf(out, n, "(none)"); return; }
    switch (vk) {
    case VK_SHIFT:   snprintf(out, n, "Shift"); return;
    case VK_CONTROL: snprintf(out, n, "Ctrl"); return;
    case VK_BACK:    snprintf(out, n, "Backspace"); return;
    case VK_RETURN:  snprintf(out, n, "Enter"); return;
    case VK_SPACE:   snprintf(out, n, "Space"); return;
    }
    sc = (LONG)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16;
    switch (vk) {                                    /* the extended keys */
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
    case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        sc |= 1L << 24;
    }
    if (!GetKeyNameTextA(sc, out, (int)n) || !out[0])
        snprintf(out, n, "Key %d", vk);
}

static void ini_path(char *out, size_t n, int create_dir)
{
    char base[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base))
        strcpy(base, ".");
    snprintf(out, n, "%s\\NHLHitzPro", base);
    if (create_dir) CreateDirectoryA(out, NULL);
    snprintf(out, n, "%s\\NHLHitzPro\\keymap.ini", base);
}

static void load_keys(void)
{
    char path[MAX_PATH], key[48];
    int i;
    ini_path(path, sizeof path, 0);
    for (i = 0; i < NHL_KEYMAP_COUNT; i++) {
        int k0, k1;
        snprintf(key, sizeof key, "%s", IDS[i]);
        k0 = (int)GetPrivateProfileIntA("keys", key, -1, path);
        snprintf(key, sizeof key, "%s_alt", IDS[i]);
        k1 = (int)GetPrivateProfileIntA("keys", key, -1, path);
        s_key[i][0] = (k0 >= 0 && k0 < 256) ? k0 : DEFAULTS[i][0];
        s_key[i][1] = (k1 >= 0 && k1 < 256) ? k1 : DEFAULTS[i][1];
    }
}

static int save_keys(void)
{
    char path[MAX_PATH], key[48], val[16];
    int i, ok = 1;
    ini_path(path, sizeof path, 1);
    for (i = 0; i < NHL_KEYMAP_COUNT; i++) {
        snprintf(key, sizeof key, "%s", IDS[i]);
        snprintf(val, sizeof val, "%d", s_key[i][0]);
        ok &= WritePrivateProfileStringA("keys", key, val, path);
        snprintf(key, sizeof key, "%s_alt", IDS[i]);
        snprintf(val, sizeof val, "%d", s_key[i][1]);
        ok &= WritePrivateProfileStringA("keys", key, val, path);
    }
    return ok;
}

static void refresh_row(int i)
{
    char t[64];
    key_name(s_key[i][0], t, sizeof t);
    ListView_SetItemText(s_list, i, 1, t);
    key_name(s_key[i][1], t, sizeof t);
    ListView_SetItemText(s_list, i, 2, t);
}

static void refresh_all(void)
{
    int i;
    for (i = 0; i < NHL_KEYMAP_COUNT; i++) refresh_row(i);
}

static void status(const char *s) { SetWindowTextA(s_status, s); }

static void begin_wait(int slot)
{
    char m[160];
    int row = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
    if (row < 0) { status("Select an action in the list first."); return; }
    s_wait_row = row; s_wait_slot = slot;
    snprintf(m, sizeof m, "Press the new %s for \"%s\"  (Esc cancels).", slot ? "alternate key" : "key", LABELS[row]);
    status(m);
    SetFocus(s_main);                  /* the key goes to this window, not a button */
}

static void assign(int vk)
{
    char m[200], n[64];
    int i, s, moved = 0;
    if (vk == VK_ESCAPE) { s_wait_row = -1; status("Cancelled."); return; }
    if (vk == VK_F11 || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) {
        key_name(vk, n, sizeof n);
        snprintf(m, sizeof m, "%s is used by the game itself (fullscreen, Alt+Enter, Esc) and can't be assigned. Press another key.",
                 vk == VK_F11 ? "F11" : n);
        status(m);
        return;
    }
    for (i = 0; i < NHL_KEYMAP_COUNT; i++)           /* a key drives one action: take it from the old one */
        for (s = 0; s < 2; s++)
            if (s_key[i][s] == vk && !(i == s_wait_row && s == s_wait_slot)) {
                s_key[i][s] = 0;
                refresh_row(i);
                key_name(vk, n, sizeof n);
                snprintf(m, sizeof m, "%s was moved here from \"%s\".", n, LABELS[i]);
                status(m);
                moved = 1;
            }
    s_key[s_wait_row][s_wait_slot] = vk;
    refresh_row(s_wait_row);
    if (!moved) {
        key_name(vk, n, sizeof n);
        snprintf(m, sizeof m, "\"%s\" is now %s.", LABELS[s_wait_row], n);
        status(m);
    }
    s_wait_row = -1;
    s_dirty = 1;
}

static void do_reset(void)
{
    int i;
    for (i = 0; i < NHL_KEYMAP_COUNT; i++) { s_key[i][0] = DEFAULTS[i][0]; s_key[i][1] = DEFAULTS[i][1]; }
    refresh_all();
    s_dirty = 1;
    s_wait_row = -1;
    status("Default keys restored. Click Save to keep them.");
}

static int do_save(void)
{
    if (!save_keys()) { MessageBoxA(s_main, "The settings could not be saved.", "NHL Hitz Pro", MB_ICONERROR); return 0; }
    s_dirty = 0;
    status("Saved. The new keys apply the next time the game starts.");
    return 1;
}

static void make_ui(HWND h)
{
    LVCOLUMNA c;
    LVITEMA it;
    int i;
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    struct { int id; const char *t; int x; int w; } b[] = {
        { ID_CHANGE, "Change key", 10, 100 }, { ID_ALT, "Change alternate", 116, 120 },
        { ID_CLEAR, "Clear", 242, 70 }, { ID_RESET, "Reset to defaults", 318, 120 },
        { ID_SAVE, "Save", 444, 70 }, { ID_CLOSE, "Close", 520, 70 } };

    CreateWindowExA(0, "STATIC", "Keyboard controls. Select an action, click Change key (or double-click it), then press the key you want.",
                    WS_CHILD | WS_VISIBLE, 10, 8, 580, 34, h, NULL, NULL, NULL);
    s_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             10, 46, 580, 440, h, (HMENU)(INT_PTR)ID_LIST, NULL, NULL);
    ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    memset(&c, 0, sizeof c);
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = "Action"; c.cx = 220; SendMessageA(s_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&c);
    c.pszText = "Key"; c.cx = 170; SendMessageA(s_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&c);
    c.pszText = "Alternate key"; c.cx = 170; SendMessageA(s_list, LVM_INSERTCOLUMNA, 2, (LPARAM)&c);
    for (i = 0; i < NHL_KEYMAP_COUNT; i++) {
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (char *)LABELS[i];
        SendMessageA(s_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
    }
    refresh_all();
    ListView_SetItemState(s_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    s_status = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE, 10, 494, 580, 20, h, (HMENU)(INT_PTR)ID_STATUS, NULL, NULL);
    for (i = 0; i < 6; i++) {
        HWND hb = CreateWindowExA(0, "BUTTON", b[i].t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  b[i].x, 520, b[i].w, 28, h, (HMENU)(INT_PTR)b[i].id, NULL, NULL);
        SendMessageA(hb, WM_SETFONT, (WPARAM)f, TRUE);
    }
    SendMessageA(s_list, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageA(s_status, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageA(GetWindow(h, GW_CHILD), WM_SETFONT, (WPARAM)f, TRUE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        s_main = h;
        make_ui(h);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_CHANGE: begin_wait(0); return 0;
        case ID_ALT:    begin_wait(1); return 0;
        case ID_CLEAR: {
            int row = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
            if (row < 0) { status("Select an action in the list first."); return 0; }
            s_key[row][0] = s_key[row][1] = 0;
            refresh_row(row);
            s_dirty = 1;
            status("Cleared. That action has no key until you assign one.");
            return 0;
        }
        case ID_RESET: do_reset(); return 0;
        case ID_SAVE:  do_save(); return 0;
        case ID_CLOSE: PostMessageA(h, WM_CLOSE, 0, 0); return 0;
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)l)->idFrom == ID_LIST && ((NMHDR *)l)->code == NM_DBLCLK) {
            NMITEMACTIVATE *a = (NMITEMACTIVATE *)l;
            LVHITTESTINFO ht;
            memset(&ht, 0, sizeof ht);
            ht.pt = a->ptAction;
            if (ListView_SubItemHitTest(s_list, &ht) >= 0 && ht.iItem >= 0) {
                ListView_SetItemState(s_list, ht.iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                begin_wait(ht.iSubItem == 2 ? 1 : 0);
            }
        }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (s_wait_row >= 0) {
            if (!(l & (1L << 30)))
                assign((int)w);
            return 0;                       /* no beeps, no menu keys, nothing leaks through */
        }
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (s_wait_row >= 0) return 0;
        break;
    case WM_CLOSE:
        if (s_dirty) {
            int r = MessageBoxA(h, "Save your changes before closing?", "NHL Hitz Pro", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES && !do_save()) return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_LISTVIEW_CLASSES };
    RECT r = { 0, 0, 600, 560 };
    (void)prev; (void)cmd;

    InitCommonControlsEx(&ic);
    load_keys();
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "NhlHitzConfig";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    CreateWindowExA(0, wc.lpszClassName, "NHL Hitz Pro - Settings",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(s_main, show);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (s_wait_row < 0 && IsDialogMessageA(s_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
