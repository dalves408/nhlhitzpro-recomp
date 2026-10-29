/* nhl_import: copies the game's data files out of the player's own copy of the disc.
 *
 *   nhl_import <source> <dest_dir>
 *
 * <source> is an Xbox disc image (.iso: a full Redump image or a plain XISO) or a
 * folder holding the game's files (the disc's root, with default.xbe). Only the files
 * listed in manifest.h are written to <dest_dir>, after default.xbe matches the
 * version this build was made from. Progress goes to stdout as "PROGRESS done total".
 *
 * Exit codes: 0 ok, 2 source not found, 3 not an Xbox disc image, 4 wrong game or
 * version (default.xbe differs), 5 files missing or the wrong size, 6 cannot write. */
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "manifest.h"

#define SECTOR 2048ull
static const unsigned long long XDVDFS_OFFSETS[] = { 0, 0x18300000ull, 0x0FD90000ull, 0x02080000ull };

static unsigned long long g_done;
static unsigned g_found;
static int g_found_flag[MANIFEST_COUNT];
static WCHAR g_dest[MAX_PATH];
static HANDLE g_img = INVALID_HANDLE_VALUE;
static unsigned long long g_base;
static int g_err;
static int g_hash_only;      /* 1 while extracting only default.xbe to hash it */

static int manifest_find(const char *rel)
{
    int i;
    for (i = 0; i < MANIFEST_COUNT; i++)
        if (_stricmp(MANIFEST[i].path, rel) == 0)
            return i;
    return -1;
}

static void progress(void)
{
    printf("PROGRESS %llu %llu\n", g_done, (unsigned long long)MANIFEST_BYTES);
    fflush(stdout);
}

static int make_dirs_for(const WCHAR *file)
{
    WCHAR dir[MAX_PATH], *sl;
    wcscpy_s(dir, MAX_PATH, file);
    sl = wcsrchr(dir, L'\\');
    if (!sl) return 1;
    *sl = 0;
    return SHCreateDirectoryExW(NULL, dir, NULL) == ERROR_SUCCESS ||
           GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES;
}

static void dest_path(const char *rel, WCHAR *out)
{
    WCHAR w[MAX_PATH];
    WCHAR *p;
    MultiByteToWideChar(CP_ACP, 0, rel, -1, w, MAX_PATH);
    for (p = w; *p; p++) if (*p == L'/') *p = L'\\';
    swprintf_s(out, MAX_PATH, L"%s\\%s", g_dest, w);
}

static int sha256_file(HANDLE h, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hh;
    static unsigned char buf[1 << 20];
    DWORD n;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return 0;
    if (!BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0)) {
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        while (ReadFile(h, buf, sizeof buf, &n, NULL) && n)
            BCryptHashData(hh, buf, n, 0);
        ok = !BCryptFinishHash(hh, out, 32, 0);
        BCryptDestroyHash(hh);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static int xbe_ok(const WCHAR *path)
{
    unsigned char h[32];
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    int ok;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = sha256_file(f, h) && memcmp(h, XBE_SHA256, 32) == 0;
    CloseHandle(f);
    return ok;
}

/* copy len bytes from `in` (already positioned) to a new file at dest */
static int write_out(const char *rel, HANDLE in, unsigned long long len, int count_progress)
{
    static unsigned char buf[1 << 20];
    WCHAR dst[MAX_PATH];
    HANDLE out;
    unsigned long long left = len;
    DWORD n, w;
    dest_path(rel, dst);
    if (!make_dirs_for(dst)) return 0;
    out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) return 0;
    while (left) {
        DWORD want = left > sizeof buf ? (DWORD)sizeof buf : (DWORD)left;
        if (!ReadFile(in, buf, want, &n, NULL) || n == 0) break;
        if (!WriteFile(out, buf, n, &w, NULL) || w != n) { CloseHandle(out); return 0; }
        left -= n;
        if (count_progress) { g_done += n; progress(); }
    }
    CloseHandle(out);
    return left == 0;
}

/* ---- disc image (XDVDFS) -------------------------------------------------- */
static int img_read(unsigned long long off, void *buf, DWORD n)
{
    LARGE_INTEGER p;
    DWORD got;
    p.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(g_img, p, NULL, FILE_BEGIN) && ReadFile(g_img, buf, n, &got, NULL) && got == n;
}

static void walk(unsigned long long dir_sector, unsigned dir_size, unsigned off, const char *prefix)
{
    unsigned char *d;
    unsigned char *e;
    unsigned left, right, sector, size, attr, nl;
    char name[256], rel[600];
    if (g_err || dir_size == 0) return;
    d = (unsigned char *)malloc(dir_size);
    if (!d || !img_read(g_base + dir_sector * SECTOR, d, dir_size)) { free(d); g_err = 3; return; }
    {
        /* iterative-free tree walk: the directory is a binary tree of entries */
        struct { unsigned pos; int state; } st[512];
        int sp = 0;
        st[0].pos = off; st[0].state = 0; sp = 1;
        while (sp && !g_err) {
            unsigned pos = st[sp - 1].pos;
            int s = st[sp - 1].state;
            if (pos + 14 > dir_size) { sp--; continue; }
            e = d + pos;
            left = e[0] | (e[1] << 8);
            right = e[2] | (e[3] << 8);
            if (left == 0xFFFF) { sp--; continue; }
            if (s == 0) {
                st[sp - 1].state = 1;
                if (left && sp < 512) { st[sp].pos = left * 4; st[sp].state = 0; sp++; }
                continue;
            }
            if (s == 1) {
                st[sp - 1].state = 2;
                sector = e[4] | (e[5] << 8) | (e[6] << 16) | ((unsigned)e[7] << 24);
                size = e[8] | (e[9] << 8) | (e[10] << 16) | ((unsigned)e[11] << 24);
                attr = e[12];
                nl = e[13];
                memcpy(name, e + 14, nl);
                name[nl] = 0;
                snprintf(rel, sizeof rel, "%s%s", prefix, name);
                if (attr & 0x10) {
                    char sub[600];
                    snprintf(sub, sizeof sub, "%s/", rel);
                    walk(sector, size, 0, sub);
                } else {
                    int mi = manifest_find(rel);
                    if (mi >= 0 && (!g_hash_only || _stricmp(rel, "default.xbe") == 0)) {
                        LARGE_INTEGER p;
                        p.QuadPart = (LONGLONG)(g_base + (unsigned long long)sector * SECTOR);
                        SetFilePointerEx(g_img, p, NULL, FILE_BEGIN);
                        if (size != MANIFEST[mi].size) { g_err = 5; }
                        else if (!write_out(rel, g_img, size, !g_hash_only)) { g_err = 6; }
                        else if (!g_hash_only && !g_found_flag[mi]) { g_found_flag[mi] = 1; g_found++; }
                        else if (g_hash_only) g_found_flag[mi] = 1;
                    }
                }
                continue;
            }
            /* s == 2: right subtree */
            sp--;
            if (right && sp < 511) { st[sp].pos = right * 4; st[sp].state = 0; sp++; }
        }
    }
    free(d);
}

static int find_xdvdfs(unsigned long long *root_sector, unsigned *root_size)
{
    unsigned char v[24];
    size_t i;
    for (i = 0; i < sizeof XDVDFS_OFFSETS / sizeof *XDVDFS_OFFSETS; i++) {
        if (img_read(XDVDFS_OFFSETS[i] + 32 * SECTOR, v, 24) && memcmp(v, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
            g_base = XDVDFS_OFFSETS[i];
            *root_sector = v[20] | (v[21] << 8) | (v[22] << 16) | ((unsigned)v[23] << 24);
            if (!img_read(g_base + 32 * SECTOR + 24, root_size, 4)) return 0;
            return 1;
        }
    }
    return 0;
}

/* ---- folder --------------------------------------------------------------- */
static int copy_folder(const WCHAR *root)
{
    int i;
    for (i = 0; i < MANIFEST_COUNT; i++) {
        WCHAR src[MAX_PATH], rel[MAX_PATH], *p;
        HANDLE in;
        LARGE_INTEGER sz;
        MultiByteToWideChar(CP_ACP, 0, MANIFEST[i].path, -1, rel, MAX_PATH);
        for (p = rel; *p; p++) if (*p == L'/') *p = L'\\';
        swprintf_s(src, MAX_PATH, L"%s\\%s", root, rel);
        in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (in == INVALID_HANDLE_VALUE) continue;
        if (GetFileSizeEx(in, &sz) && (unsigned long long)sz.QuadPart == MANIFEST[i].size) {
            if (!write_out(MANIFEST[i].path, in, MANIFEST[i].size, 1)) { CloseHandle(in); return 6; }
            g_found_flag[i] = 1; g_found++;
        }
        CloseHandle(in);
    }
    return 0;
}

/* the folder that holds default.xbe: the folder itself or one of its children */
static int find_root(const WCHAR *dir, WCHAR *root)
{
    WCHAR pat[MAX_PATH], t[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(t, MAX_PATH, L"%s\\default.xbe", dir);
    if (GetFileAttributesW(t) != INVALID_FILE_ATTRIBUTES) { wcscpy_s(root, MAX_PATH, dir); return 1; }
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
            swprintf_s(t, MAX_PATH, L"%s\\%s\\default.xbe", dir, fd.cFileName);
            if (GetFileAttributesW(t) != INVALID_FILE_ATTRIBUTES) {
                swprintf_s(root, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
                FindClose(h);
                return 1;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    DWORD attr;
    WCHAR root[MAX_PATH], t[MAX_PATH];
    int i, missing = 0;

    if (argc < 3) { printf("usage: nhl_import <iso or folder> <dest folder>\n"); return 2; }
    wcscpy_s(g_dest, MAX_PATH, argv[2]);
    {
        size_t n = wcslen(g_dest);
        while (n > 3 && (g_dest[n - 1] == L'\\' || g_dest[n - 1] == L'/')) g_dest[--n] = 0;
    }
    attr = GetFileAttributesW(argv[1]);
    if (attr == INVALID_FILE_ATTRIBUTES) { printf("ERROR The disc image or folder was not found.\n"); return 2; }

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        int r;
        if (!find_root(argv[1], root)) { printf("ERROR default.xbe was not found in that folder.\n"); return 4; }
        swprintf_s(t, MAX_PATH, L"%s\\default.xbe", root);
        if (!xbe_ok(t)) { printf("ERROR This is not the supported version of NHL Hitz Pro (default.xbe differs).\n"); return 4; }
        r = copy_folder(root);
        if (r) { printf("ERROR Could not write to the install folder.\n"); return r; }
    } else {
        unsigned long long rs;
        unsigned rsz;
        g_img = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (g_img == INVALID_HANDLE_VALUE) { printf("ERROR The disc image could not be opened.\n"); return 2; }
        if (!find_xdvdfs(&rs, &rsz)) { printf("ERROR This is not an Xbox disc image.\n"); return 3; }
        /* pass 1: default.xbe only, to check the version before copying 1.5 GB */
        g_hash_only = 1;
        walk(rs, rsz, 0, "");
        if (g_err == 6) { printf("ERROR Could not write to the install folder.\n"); return 6; }
        swprintf_s(t, MAX_PATH, L"%s\\default.xbe", g_dest);
        if (!g_found_flag[manifest_find("default.xbe")] || !xbe_ok(t)) {
            DeleteFileW(t);
            printf("ERROR This is not the supported version of NHL Hitz Pro (default.xbe differs).\n");
            return 4;
        }
        g_err = 0; g_hash_only = 0;
        memset(g_found_flag, 0, sizeof g_found_flag);
        walk(rs, rsz, 0, "");
        if (g_err == 6) { printf("ERROR Could not write to the install folder.\n"); return 6; }
        if (g_err) { printf("ERROR The disc image is damaged or incomplete.\n"); return 5; }
        CloseHandle(g_img);
    }
    for (i = 0; i < MANIFEST_COUNT; i++)
        if (!g_found_flag[i]) { if (missing++ < 5) printf("MISSING %s\n", MANIFEST[i].path); }
    if (missing) { printf("ERROR %d game files are missing or the wrong size (bad or incomplete dump).\n", missing); return 5; }
    printf("OK %u files\n", g_found);
    return 0;
}
