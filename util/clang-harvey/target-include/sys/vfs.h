/* APEX has no filesystem statistics interface.  LLVM includes <sys/vfs.h>
 * on Linux-ish targets for getPathDiskUsage(); provide the type and fail
 * the call rather than failing the build. */
#ifndef HARVEY_SYS_VFS_H
#define HARVEY_SYS_VFS_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct statfs {
    long   f_type;
    long   f_bsize;
    long   f_frsize;
    long   f_blocks;
    long   f_bfree;
    long   f_bavail;
    long   f_files;
    long   f_ffree;
    long   f_namelen;
    long   f_flags;
    long   f_fsid[2];
};

int statfs(const char *, struct statfs *);
int fstatfs(int, struct statfs *);

#ifdef __cplusplus
}
#endif

#endif
