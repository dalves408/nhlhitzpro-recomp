#ifndef HITZ_LINUX_GUEST_MAPPING_H
#define HITZ_LINUX_GUEST_MAPPING_H
#include <stddef.h>
#include "platform/xbox_winnt.h"
void *hitz_linux_reserve_guest(size_t);
void *hitz_linux_map_guest(HANDLE,size_t,void *);
void *hitz_linux_commit_guest(void *,size_t);
void hitz_linux_release_guest(void);
#endif
