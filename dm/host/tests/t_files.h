// Folders for the host tests, made and removed through the file system's calls rather than a shell (system("mkdir -p …") / system("rm -rf …"): clang-tidy's
// bugprone-command-processor, and a quoting bug away from removing the wrong folder)
#pragma once
#include <errno.h>
#include <removefile.h>
#include <string.h>
#include <sys/stat.h>

// mkdir -p: path and every missing folder above it. 0 on success, also when it is there already
static inline int t_mkdirs(const char *path) {
    char p[1024];
    if (!*path || strlcpy(p, path, sizeof p) >= sizeof p) { errno = ENAMETOOLONG; return -1; }
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; if (mkdir(p, 0755) && errno != EEXIST) return -1; *s = '/'; }
    return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}

// mkdir -p of the folder that holds path
static inline int t_mkdirs_for(const char *path) {
    char d[1024];
    if (strlcpy(d, path, sizeof d) >= sizeof d) { errno = ENAMETOOLONG; return -1; }
    char *s = strrchr(d, '/');
    if (!s || s == d) return 0;
    *s = 0;
    return t_mkdirs(d);
}

// rm -rf: path and everything under it (a symbolic link is removed, never followed). 0 on success, also when nothing is there
static inline int t_rmtree(const char *path) {
    return removefile(path, NULL, REMOVEFILE_RECURSIVE) && errno != ENOENT ? -1 : 0;
}
