/* Project mapping adapter. Keep the toolkit unchanged, including its refusal
 * to replace unrelated host mappings. Only our own PROT_NONE guest reservation
 * can be replaced atomically with shared RAM views or device pages. */
#include "../../../tools/xboxrecomp/src/platform/win32_compat.c"
#include "linux_guest_mapping.h"
static void *s_guest_reservation;
static size_t s_guest_reservation_size;
static int owned_guest_range(void *address,size_t size)
{
    uintptr_t a=(uintptr_t)address,b=(uintptr_t)s_guest_reservation;
    return s_guest_reservation&&size&&size<=s_guest_reservation_size&&a>=b
        &&a-b<=s_guest_reservation_size-size;
}
void *hitz_linux_reserve_guest(size_t size)
{
    if(s_guest_reservation)return NULL;
    void *p=mmap(NULL,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    if(p==MAP_FAILED)return NULL;
    s_guest_reservation=p;s_guest_reservation_size=size;return p;
}
void *hitz_linux_map_guest(HANDLE mapping,size_t size,void *address)
{
    w32_object *o=(w32_object *)mapping;
    if(!owned_guest_range(address,size)||!o||o->kind!=K_FILEMAP||size>o->map_size){SetLastError(ERROR_INVALID_ADDRESS);return NULL;}
    void *p=mmap(address,size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,o->fd,0);
    if(p==MAP_FAILED){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
    view_register(p,size);return p;
}
void *hitz_linux_commit_guest(void *address,size_t size)
{
    if(!owned_guest_range(address,size)){SetLastError(ERROR_INVALID_ADDRESS);return NULL;}
    void *p=mmap(address,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    if(p==MAP_FAILED){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
    view_register(p,size);return p;
}
void hitz_linux_release_guest(void)
{
    if(s_guest_reservation)munmap(s_guest_reservation,s_guest_reservation_size);
    s_guest_reservation=NULL;s_guest_reservation_size=0;
}
