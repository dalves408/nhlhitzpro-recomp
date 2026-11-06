/* Xbox paths are case insensitive. Resolve existing components without
 * renaming disc assets. Raw partitions use sparse anonymous memfds; loose
 * saves persist normally, and no Partition*.img files reach the filesystem. */
#include "filesystem_linux.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
static char save_root[PATH_MAX];
static int raw_fds[6]={-1,-1,-1,-1,-1,-1};
static pthread_mutex_t raw_lock=PTHREAD_MUTEX_INITIALIZER;
void hitz_linux_set_save_root(const char *root){snprintf(save_root,sizeof(save_root),"%s",root);}
static int raw_partition(const char *path)
{
    size_t n=strlen(save_root);
    if(!n||strncmp(path,save_root,n)||path[n]!='/')return -1;
    const char *name=path+n+1;
    if(strlen(name)==14&&!strncasecmp(name,"Partition",9)&&name[9]>='0'&&name[9]<='5'&&!strcasecmp(name+10,".img"))return name[9]-'0';
    return -1;
}
static int raw_fd(int part)
{
    pthread_mutex_lock(&raw_lock);
    if(raw_fds[part]<0){
        static const uint64_t sectors[]={0x009c4800ull,0x00465400ull,0x000fa000ull,0x00177000ull,0x00177000ull,0x00177000ull};
        char name[32];snprintf(name,sizeof(name),"hitz-partition-%d",part);
        int fd=memfd_create(name,MFD_CLOEXEC|MFD_ALLOW_SEALING);
        if(fd>=0&&(ftruncate(fd,(off_t)(sectors[part]*512))||fcntl(fd,F_ADD_SEALS,F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL))){close(fd);fd=-1;}
        raw_fds[part]=fd;
    }
    int fd=raw_fds[part];
    pthread_mutex_unlock(&raw_lock);return fd;
}
static int resolve(const char *path,char out[PATH_MAX])
{
    if(strlen(path)>=PATH_MAX){errno=ENAMETOOLONG;return 0;}
    char copy[PATH_MAX];strcpy(copy,path);out[0]=0;
    if(path[0]=='/')strcpy(out,"/");
    char *state=NULL;
    for(char *part=strtok_r(copy,"/",&state);part;part=strtok_r(NULL,"/",&state)){
        size_t base=strlen(out);if(base&&out[base-1]!='/')out[base++]='/';
        if(base+strlen(part)>=PATH_MAX){errno=ENAMETOOLONG;return 0;}
        strcpy(out+base,part);
        struct stat st;
        if(lstat(out,&st)==0)continue;
        out[base]=0;DIR *dir=opendir(base?out:".");
        const char *match=part;char found[NAME_MAX+1];
        if(dir){struct dirent *entry;while((entry=readdir(dir)))if(!strcasecmp(part,entry->d_name)){
            snprintf(found,sizeof(found),"%s",entry->d_name);match=found;break;}
            closedir(dir);}
        if(base+strlen(match)>=PATH_MAX){errno=ENAMETOOLONG;return 0;}
        strcpy(out+base,match);
    }
    if(!out[0])strcpy(out,".");return 1;
}
int hitz_linux_open(const char *path,int flags,...)
{
    mode_t mode=0;if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=(mode_t)va_arg(ap,int);va_end(ap);}
    int part=raw_partition(path);
    if(part>=0){int fd=raw_fd(part);if(fd<0)return -1;
        if(flags&O_DIRECTORY){errno=ENOTDIR;return -1;}
        char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);
        /* New open description: independent cursor, shared partition bytes. */
        return open(proc,(flags&~(O_CREAT|O_EXCL|O_TRUNC))|O_CLOEXEC);
    }
    char actual[PATH_MAX];return resolve(path,actual)?open(actual,flags,mode):-1;
}
int hitz_linux_stat(const char *path,struct stat *st)
{int p=raw_partition(path);if(p>=0){int fd=raw_fd(p);return fd<0?-1:fstat(fd,st);}char actual[PATH_MAX];return resolve(path,actual)?stat(actual,st):-1;}
int hitz_linux_unlink(const char *path)
{char actual[PATH_MAX];return resolve(path,actual)?unlink(actual):-1;}
int hitz_linux_rename(const char *src,const char *dst)
{char a[PATH_MAX],b[PATH_MAX];return resolve(src,a)&&resolve(dst,b)?rename(a,b):-1;}
DIR *hitz_linux_opendir(const char *path)
{char actual[PATH_MAX];return resolve(path,actual)?opendir(actual):NULL;}
int hitz_linux_mkdir(const char *path,mode_t mode)
{char actual[PATH_MAX];return resolve(path,actual)?mkdir(actual,mode):-1;}
