#ifndef HITZ_FILESYSTEM_LINUX_H
#define HITZ_FILESYSTEM_LINUX_H
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
void hitz_linux_set_save_root(const char *);
int hitz_linux_open(const char *,int,...);
int hitz_linux_stat(const char *,struct stat *);
int hitz_linux_unlink(const char *);
int hitz_linux_rename(const char *,const char *);
int hitz_linux_mkdir(const char *,mode_t);
DIR *hitz_linux_opendir(const char *);
#ifdef HITZ_WRAP_FILESYSTEM
#define open(...) hitz_linux_open(__VA_ARGS__)
#define stat(path,buf) hitz_linux_stat(path,buf)
#define unlink(path) hitz_linux_unlink(path)
#define rename(src,dst) hitz_linux_rename(src,dst)
#define opendir(path) hitz_linux_opendir(path)
#define mkdir(path,mode) hitz_linux_mkdir(path,mode)
#endif
#endif
