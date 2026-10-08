// <sys/utsname.h> for the NetSurf PAX: uname() (pax_syscalls.c) names
// the platform in NetSurf's user agent string.
#ifndef PAX_SYS_UTSNAME_H
#define PAX_SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif
