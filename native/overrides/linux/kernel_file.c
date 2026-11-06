/* Use the toolkit filesystem with project-owned, memory-only raw devices. */
#include "kernel.h"
#ifdef _WIN32
#include "hitz_virtual_disk.h"
#elif defined(__linux__)
/* Failed guest opens may legally be followed by a size/query call. Win32
 * rejects INVALID_HANDLE_VALUE; never dereference it as a POSIX object. */
static int hitz_checked_handle_fd(HANDLE handle)
{ return handle && handle!=INVALID_HANDLE_VALUE ? w32_handle_fd(handle) : -1; }
static const char *hitz_checked_handle_path(HANDLE handle)
{ return handle && handle!=INVALID_HANDLE_VALUE ? w32_handle_path(handle) : NULL; }
#define w32_handle_fd hitz_checked_handle_fd
#define w32_handle_path hitz_checked_handle_path
#define HITZ_WRAP_FILESYSTEM
#include "../../src/filesystem_linux.h"
#endif
#include "../../../tools/xboxrecomp/src/kernel/kernel_file.c"
