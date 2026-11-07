/* Raw Xbox devices have fixed geometry and sparse RAM contents. The tiny
 * pagefile-backed section is an open-file description, so native duplicated
 * handles share their cursor and remain valid after the original is closed.
 * Sector data belongs to the emulated disk for the process lifetime. No host
 * files are opened and no partition-sized memory allocation is made. */
#include <stdlib.h>
#include <stdint.h>
#include <wchar.h>
#include <string.h>

typedef struct HitzDiskOpen {
    uint64_t magic, position;
    unsigned part;
} HitzDiskOpen;
typedef struct HitzDiskBlock {
    struct HitzDiskBlock *next;
    uint64_t index;
    unsigned char data[4096];
} HitzDiskBlock;
static HitzDiskBlock *s_disk_blocks[10];
static SRWLOCK s_disk_lock = SRWLOCK_INIT;
static const uint64_t s_disk_bytes[10] = {
    0x009C4800ull * 512, 0x00465400ull * 512, 0x000FA000ull * 512,
    0x00177000ull * 512, 0x00177000ull * 512, 0x00177000ull * 512
};
#define HITZ_DISK_MAGIC 0x4849545A4449534Bull
extern void xbox_build_partition_table(unsigned char sector[512]);
extern void hitz_disk_directory(unsigned part, WCHAR out[MAX_PATH]);

static HitzDiskOpen *hitz_disk_view(HANDLE h)
{
    HitzDiskOpen *v;
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_UNKNOWN)
        return NULL;
    v = (HitzDiskOpen *)MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(*v));
    if (v && (v->magic != HITZ_DISK_MAGIC || v->part >= 10)) {
        UnmapViewOfFile(v); v = NULL;
    }
    return v;
}

BOOL hitz_disk_size(HANDLE h, PLARGE_INTEGER size)
{
    HitzDiskOpen *v = hitz_disk_view(h);
    if (!v) return FALSE;
    size->QuadPart = s_disk_bytes[v->part];
    UnmapViewOfFile(v);
    return TRUE;
}

static HANDLE WINAPI hitz_disk_create(LPCWSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file)
{
    static const WCHAR prefix[] = L"HitzVirtualDisk:";
    const size_t n = (sizeof(prefix) / sizeof(prefix[0])) - 1;
    HANDLE h;
    HitzDiskOpen *v;
    if (wcsncmp(path, prefix, n) || path[n] < L'0' || path[n] > L'9' || path[n+1])
        return CreateFileW(path, access, share, sa, disposition, flags, template_file);
    if (flags & FILE_FLAG_BACKUP_SEMANTICS) {
        WCHAR directory[MAX_PATH];
        hitz_disk_directory(path[n] - L'0', directory);
        return CreateFileW(directory, access, share, sa, disposition, flags, template_file);
    }
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                          sizeof(HitzDiskOpen), NULL);
    if (!h) return INVALID_HANDLE_VALUE;
    v = (HitzDiskOpen *)MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(*v));
    if (!v) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    v->magic = HITZ_DISK_MAGIC;
    v->position = 0;
    v->part = path[n] - L'0';
    UnmapViewOfFile(v);
    return h;
}

static BOOL hitz_disk_transfer(HitzDiskOpen *v, void *buffer, DWORD length,
                             LPDWORD transferred, LPOVERLAPPED ov, BOOL write)
{
    uint64_t pos = ov ? ((uint64_t)ov->OffsetHigh << 32) | ov->Offset : 0;
    DWORD done = 0;
    BOOL ok = TRUE;
    AcquireSRWLockExclusive(&s_disk_lock);
    /* Read the shared cursor while locked. Explicit offsets also advance it,
     * matching the synchronous host file handles used by the runtime. */
    if (!ov) pos = v->position;
    if (pos > s_disk_bytes[v->part] || length > s_disk_bytes[v->part] - pos) {
        if (write) { ok = FALSE; SetLastError(ERROR_DISK_FULL); goto finish; }
        length = pos >= s_disk_bytes[v->part] ? 0 : (DWORD)(s_disk_bytes[v->part] - pos);
    }
    while (done < length) {
        uint64_t index = pos / 4096;
        unsigned offset = (unsigned)(pos % 4096);
        DWORD count = 4096 - offset;
        HitzDiskBlock *b = s_disk_blocks[v->part];
        if (count > length - done) count = length - done;
        while (b && b->index != index) b = b->next;
        if (!b && write) {
            b = (HitzDiskBlock *)calloc(1, sizeof(*b));
            if (!b) { ok = FALSE; SetLastError(ERROR_NOT_ENOUGH_MEMORY); break; }
            b->index = index;
            b->next = s_disk_blocks[v->part];
            s_disk_blocks[v->part] = b;
            if (v->part == 0 && index == 0) xbox_build_partition_table(b->data + 0x800);
        }
        if (write) memcpy(b->data + offset, (unsigned char *)buffer + done, count);
        else if (b) memcpy((unsigned char *)buffer + done, b->data + offset, count);
        else {
            unsigned char empty[4096] = {0};
            if (v->part == 0 && index == 0) xbox_build_partition_table(empty + 0x800);
            memcpy((unsigned char *)buffer + done, empty + offset, count);
        }
        pos += count; done += count;
    }
    v->position = pos;
finish:
    if (transferred) *transferred = done;
    if (ov) { ov->Internal = ok ? 0 : 0xC0000001u; ov->InternalHigh = done; }
    ReleaseSRWLockExclusive(&s_disk_lock);
    UnmapViewOfFile(v);
    return ok;
}

static BOOL WINAPI hitz_disk_read(HANDLE h, LPVOID buf, DWORD n, LPDWORD done, LPOVERLAPPED ov)
{
    HitzDiskOpen *v = hitz_disk_view(h);
    return v ? hitz_disk_transfer(v, buf, n, done, ov, FALSE) : ReadFile(h, buf, n, done, ov);
}
static BOOL WINAPI hitz_disk_write(HANDLE h, LPCVOID buf, DWORD n, LPDWORD done, LPOVERLAPPED ov)
{
    HitzDiskOpen *v = hitz_disk_view(h);
    return v ? hitz_disk_transfer(v, (void *)buf, n, done, ov, TRUE) : WriteFile(h, buf, n, done, ov);
}
static BOOL WINAPI hitz_disk_seek(HANDLE h, LARGE_INTEGER distance, PLARGE_INTEGER result, DWORD method)
{
    HitzDiskOpen *v = hitz_disk_view(h);
    LONGLONG base, pos;
    if (!v) return SetFilePointerEx(h, distance, result, method);
    AcquireSRWLockExclusive(&s_disk_lock);
    base = method == FILE_BEGIN ? 0 : method == FILE_CURRENT ? v->position : s_disk_bytes[v->part];
    if (method > FILE_END || (distance.QuadPart < 0 && distance.QuadPart < -base) ||
        (distance.QuadPart > 0 && distance.QuadPart > INT64_MAX - base)) {
        ReleaseSRWLockExclusive(&s_disk_lock); UnmapViewOfFile(v);
        SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    pos = base + distance.QuadPart;
    v->position = pos;
    if (result) result->QuadPart = pos;
    ReleaseSRWLockExclusive(&s_disk_lock);
    UnmapViewOfFile(v);
    return TRUE;
}
static BOOL WINAPI hitz_disk_info(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info)
{
    LARGE_INTEGER size;
    if (!hitz_disk_size(h, &size)) return GetFileInformationByHandle(h, info);
    memset(info, 0, sizeof(*info));
    info->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    info->nFileSizeHigh = size.HighPart; info->nFileSizeLow = size.LowPart;
    info->nNumberOfLinks = 1;
    return TRUE;
}
static BOOL WINAPI hitz_disk_flush(HANDLE h)
{
    LARGE_INTEGER size;
    return hitz_disk_size(h, &size) ? TRUE : FlushFileBuffers(h);
}
static BOOL WINAPI hitz_disk_eof(HANDLE h)
{
    HitzDiskOpen *v = hitz_disk_view(h);
    BOOL ok;
    if (!v) return SetEndOfFile(h);
    AcquireSRWLockShared(&s_disk_lock);
    ok = v->position == s_disk_bytes[v->part];
    ReleaseSRWLockShared(&s_disk_lock);
    UnmapViewOfFile(v);
    if (!ok) SetLastError(ERROR_ACCESS_DENIED);
    return ok;
}
#define CreateFileW hitz_disk_create
#define ReadFile hitz_disk_read
#define WriteFile hitz_disk_write
#define SetFilePointerEx hitz_disk_seek
#define GetFileInformationByHandle hitz_disk_info
#define FlushFileBuffers hitz_disk_flush
#define SetEndOfFile hitz_disk_eof
