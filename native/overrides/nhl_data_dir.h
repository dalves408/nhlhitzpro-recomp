/* Where the Windows game keeps its own files: a portable layout.
 *
 * Everything the game writes lives in the "data" folder next to the program
 * (<install folder>\data): SaveData, settings.ini, keymap.ini, shadercache,
 * texture_pack (the player's own), debug logs and the texture download cache.
 * Nothing goes to %LOCALAPPDATA%, so the install folder can be copied, moved or
 * deleted as one piece.
 *
 * If that folder cannot be written (an install under Program Files, a
 * read-only drive), the old place, %LOCALAPPDATA%\NHLHitzPro, is used instead,
 * as in versions before 1.1.0.
 *
 * NHL_DATA_DIR (an environment variable holding a full path) overrides the
 * choice; it is for testing.
 *
 * The first time a program calls nhl_data_dir() in a data folder that has not
 * been set up yet, files from the old AppData folder are moved over (see
 * nhl_migrate_appdata). Include this header from any number of programs: all
 * functions are static.
 */
#ifndef NHL_DATA_DIR_H
#define NHL_DATA_DIR_H

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define NHL_DATA_MARKER ".moved-from-appdata"

static int nhl_dd_exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static int nhl_dd_isdir(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Create every folder on the way to `path`. */
static void nhl_dd_mkdirs(const char *path)
{
    char tmp[MAX_PATH];
    size_t i, n = strlen(path);
    if (n >= sizeof tmp) return;
    memcpy(tmp, path, n + 1);
    for (i = 3; i < n; i++) {
        if (tmp[i] == '\\') {
            tmp[i] = 0;
            CreateDirectoryA(tmp, NULL);
            tmp[i] = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
}

/* 1 when a file can be created in `dir`. */
static int nhl_dd_writable(const char *dir)
{
    char probe[MAX_PATH];
    HANDLE h;
    if (snprintf(probe, sizeof probe, "%s\\.write-test", dir) >= (int)sizeof probe) return 0;
    h = CreateFileA(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

static int nhl_dd_legacy(char *out, size_t n)
{
    char base[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 40)
        return 0;
    snprintf(out, n, "%s\\NHLHitzPro", base);
    return 1;
}

/* Move src to dst, creating the folders dst needs. */
static int nhl_dd_move(const char *src, const char *dst)
{
    char dir[MAX_PATH], *sl;
    snprintf(dir, sizeof dir, "%s", dst);
    sl = strrchr(dir, '\\');
    if (sl) { *sl = 0; nhl_dd_mkdirs(dir); }
    return MoveFileExA(src, dst, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) != 0;
}

/* Merge the folder `src` into `dst`. A file that is only in src is moved. When both have the file the newer
 * one stays in dst and the older one goes to backup (same relative path), so nothing is thrown away. */
static void nhl_dd_merge(const char *src, const char *dst, const char *backup)
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH], s[MAX_PATH], d[MAX_PATH], b[MAX_PATH];
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", src);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (snprintf(s, sizeof s, "%s\\%s", src, fd.cFileName) >= (int)sizeof s) continue;
        if (snprintf(d, sizeof d, "%s\\%s", dst, fd.cFileName) >= (int)sizeof d) continue;
        if (snprintf(b, sizeof b, "%s\\%s", backup, fd.cFileName) >= (int)sizeof b) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   /* never follow links */
            if (!nhl_dd_exists(d)) {
                if (!nhl_dd_move(s, d)) { nhl_dd_mkdirs(d); nhl_dd_merge(s, d, b); RemoveDirectoryA(s); }
            } else {
                nhl_dd_merge(s, d, b);
                RemoveDirectoryA(s);                 /* only succeeds when empty */
            }
        } else if (!nhl_dd_exists(d)) {
            nhl_dd_move(s, d);
        } else {
            WIN32_FILE_ATTRIBUTE_DATA have;
            if (GetFileAttributesExA(d, GetFileExInfoStandard, &have) &&
                CompareFileTime(&fd.ftLastWriteTime, &have.ftLastWriteTime) > 0) {
                nhl_dd_move(d, b);                   /* the old copy in dst is the older one */
                nhl_dd_move(s, d);
            } else {
                nhl_dd_move(s, b);                   /* the copy in src is the older one */
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Once per data folder: move what the game kept in %LOCALAPPDATA%\NHLHitzPro (saves, settings, keymap, the
 * player's own texture pack, debug logs) into `data`. The shader cache and the texture download cache are
 * left behind: the first is rebuilt, the second is only a download cache. */
static void nhl_migrate_appdata(const char *data)
{
    static const char *const items[] = { "SaveData", "settings.ini", "keymap.ini", "texture_pack", "debug" };
    char legacy[MAX_PATH], marker[MAX_PATH], src[MAX_PATH], dst[MAX_PATH], backup[MAX_PATH];
    size_t i;
    FILE *f;
    if (!nhl_dd_legacy(legacy, sizeof legacy) || !nhl_dd_isdir(legacy)) return;
    if (!_stricmp(legacy, data)) return;
    snprintf(marker, sizeof marker, "%s\\" NHL_DATA_MARKER, data);
    if (nhl_dd_exists(marker)) return;
    snprintf(backup, sizeof backup, "%s\\old-appdata-backup", data);
    for (i = 0; i < sizeof items / sizeof items[0]; i++) {
        snprintf(src, sizeof src, "%s\\%s", legacy, items[i]);
        snprintf(dst, sizeof dst, "%s\\%s", data, items[i]);
        if (!nhl_dd_exists(src)) continue;
        if (nhl_dd_isdir(src)) {
            char bk[MAX_PATH];
            snprintf(bk, sizeof bk, "%s\\%s", backup, items[i]);   /* an older duplicate keeps its folder level */
            if (!nhl_dd_exists(dst)) nhl_dd_mkdirs(dst);
            nhl_dd_merge(src, dst, bk);
            RemoveDirectoryA(src);
        } else if (!nhl_dd_exists(dst)) {
            nhl_dd_move(src, dst);
        } else {
            WIN32_FILE_ATTRIBUTE_DATA a, b;
            char bk[MAX_PATH];
            snprintf(bk, sizeof bk, "%s\\%s", backup, items[i]);
            if (GetFileAttributesExA(dst, GetFileExInfoStandard, &b) &&
                GetFileAttributesExA(src, GetFileExInfoStandard, &a) &&
                CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) > 0) {
                nhl_dd_move(dst, bk);
                nhl_dd_move(src, dst);
            } else {
                nhl_dd_move(src, bk);
            }
        }
    }
    RemoveDirectoryA(legacy);                        /* only succeeds when nothing is left in it */
    f = fopen(marker, "w");
    if (f) { fputs("Saves and settings were moved here from %LOCALAPPDATA%\\NHLHitzPro.\n", f); fclose(f); }
}

/* The data folder, without a trailing backslash; created if needed. Returns 0 only when no folder can be had. */
static int nhl_data_dir(char *out, size_t n)
{
    static char cached[MAX_PATH];
    static int have;
    char exe[MAX_PATH], *sl, env[MAX_PATH];
    if (have) { snprintf(out, n, "%s", cached); return 1; }

    if (GetEnvironmentVariableA("NHL_DATA_DIR", env, sizeof env) && env[0]) {
        nhl_dd_mkdirs(env);
        snprintf(cached, sizeof cached, "%s", env);
        have = 1;
        snprintf(out, n, "%s", cached);
        return 1;
    }
    if (GetModuleFileNameA(NULL, exe, sizeof exe) && (sl = strrchr(exe, '\\'))) {
        char data[MAX_PATH];
        *sl = 0;
        if (snprintf(data, sizeof data, "%s\\data", exe) < (int)sizeof data) {
            nhl_dd_mkdirs(data);
            if (nhl_dd_isdir(data) && nhl_dd_writable(data)) {
                snprintf(cached, sizeof cached, "%s", data);
                have = 1;
                nhl_migrate_appdata(cached);
                snprintf(out, n, "%s", cached);
                return 1;
            }
        }
    }
    /* Not writable: the old place. */
    if (nhl_dd_legacy(cached, sizeof cached)) {
        nhl_dd_mkdirs(cached);
        have = 1;
        snprintf(out, n, "%s", cached);
        return 1;
    }
    return 0;
}

/* data\<name>, with the data folder made. */
static int nhl_data_path(char *out, size_t n, const char *name)
{
    char d[MAX_PATH];
    if (!nhl_data_dir(d, sizeof d)) return 0;
    return snprintf(out, n, "%s\\%s", d, name) < (int)n;
}

#endif /* NHL_DATA_DIR_H */
