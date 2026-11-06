#ifndef HITZ_DISC_BUFFERING_H
#define HITZ_DISC_BUFFERING_H
#include <stdint.h>
#include <stddef.h>
/* Host cache policy only for existing read-only files on the extracted disc.
 * No writable/cache/save/directory/relative opens are changed. */
static int hitz_disc_prefix(const char *path,size_t length)
{
    static const char device[]="\\device\\cdrom0\\";
    size_t i;
    if(length>=3 && (path[0]=='d'||path[0]=='D') && path[1]==':' && (path[2]=='\\'||path[2]=='/'))return 1;
    if(length<sizeof device-1)return 0;
    for(i=0;i<sizeof device-1;i++){
        unsigned char c=(unsigned char)path[i];if(c>='A'&&c<='Z')c+=32;
        if(c!=(unsigned char)device[i])return 0;
    }
    return 1;
}
static uint32_t hitz_disc_host_options(const char *path,size_t length,uint32_t access,
                                     uint32_t disposition,uint32_t options,int enabled)
{
    const uint32_t writes=0x40000000u|0x10000000u|0x02000000u|0x000D0000u|0x00000100u|0x10u|0x4u|0x2u;
    if(enabled && disposition==1 && !(options&1u) && !(access&writes) &&
       hitz_disc_prefix(path,length))return options&~8u;
    return options;
}
#endif
