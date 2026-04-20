#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <glob.h>
#include <ftw.h>
#include <fnmatch.h>
#include <limits.h>
#include "fsutil.h"

int mkdir_p(const char *path, mode_t mode) {
    char buf[PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) return -1;
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int copy_regular_file(const char *src, const char *dest, mode_t mode) {
    int fd_in = open(src, O_RDONLY);
    if (fd_in < 0) { perror(src); return -1; }
    int fd_out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd_out < 0) { perror(dest); close(fd_in); return -1; }
    char buf[65536]; ssize_t n; int ret = 0;
    while ((n = read(fd_in, buf, sizeof(buf))) > 0) {
        if (write(fd_out, buf, n) != n) { ret = -1; break; }
    }
    close(fd_in); close(fd_out);
    return ret;
}

static void ensure_parent(const char *path) {
    char parent[PATH_MAX];
    if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent)) return;
    char *sl = strrchr(parent, '/');
    if (sl) { *sl = '\0'; mkdir_p(parent, 0755); }
}

/* ---- recursive tree copy (for directories) ---- */
typedef struct { const char *src_base; const char *dest_base; } CopyCtx;
static CopyCtx g_copy_ctx;

static int copy_tree_cb(const char *fpath, const struct stat *sb,
                        int typeflag, struct FTW *ftwbuf) {
    (void)ftwbuf;
    const char *rel = fpath + strlen(g_copy_ctx.src_base);
    if (*rel == '/') rel++;

    char dest[PATH_MAX];
    if (*rel == '\0')
        snprintf(dest, sizeof(dest), "%s", g_copy_ctx.dest_base);
    else
        snprintf(dest, sizeof(dest), "%s/%s", g_copy_ctx.dest_base, rel);

    if (typeflag == FTW_D) {
        mkdir_p(dest, sb->st_mode & 0777);
    } else if (typeflag == FTW_F) {
        ensure_parent(dest);
        copy_regular_file(fpath, dest, sb->st_mode & 0777);
    } else if (typeflag == FTW_SL) {
        char target[PATH_MAX];
        ssize_t n = readlink(fpath, target, sizeof(target) - 1);
        if (n >= 0) {
            target[n] = '\0';
            ensure_parent(dest);
            unlink(dest);
            symlink(target, dest);
        }
    }
    return 0;
}

static void copy_tree(const char *src_real, const char *dest_abs) {
    g_copy_ctx.src_base = src_real;
    g_copy_ctx.dest_base = dest_abs;
    nftw(src_real, copy_tree_cb, 64, FTW_PHYS);
}

/* ---- recursive ** glob collection ---- */
typedef struct {
    char **paths; int count; int cap;
    const char *suffix_pat;
} GlobCtx;
static GlobCtx g_glob;

static int glob_collect_cb(const char *fpath, const struct stat *sb,
                           int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag != FTW_F && typeflag != FTW_SL) return 0;

    const char *base = strrchr(fpath, '/');
    base = base ? base + 1 : fpath;
    const char *pat = g_glob.suffix_pat;
    if (*pat == '/') pat++;
    if (fnmatch(pat, base, 0) != 0) return 0;

    if (g_glob.count >= g_glob.cap) {
        int newcap = g_glob.cap ? g_glob.cap * 2 : 64;
        char **tmp = realloc(g_glob.paths, newcap * sizeof(char *));
        if (!tmp) return FTW_STOP;
        g_glob.paths = tmp; g_glob.cap = newcap;
    }
    char *copy = strdup(fpath);
    if (!copy) return FTW_STOP;
    g_glob.paths[g_glob.count++] = copy;
    return 0;
}

/* ---- main entry point ---- */
int copy_into_rootfs(const char *context_dir, const char *src_pattern,
                     const char *dest_in_root, const char *temp_root) {
    char ctx_real[PATH_MAX];
    if (!realpath(context_dir, ctx_real)) return -1;

    int dest_is_dir = (dest_in_root[strlen(dest_in_root) - 1] == '/');

    char abs_pattern[PATH_MAX * 2];
    if (src_pattern[0] == '/')
        snprintf(abs_pattern, sizeof(abs_pattern), "%s", src_pattern);
    else
        snprintf(abs_pattern, sizeof(abs_pattern), "%s/%s", ctx_real, src_pattern);

    /* Collect matched source paths */
    char **srcs = NULL; int nsrcs = 0;

    if (strstr(src_pattern, "**")) {
        char *star = strstr(abs_pattern, "**");
        char base_dir[PATH_MAX];
        size_t blen = (size_t)(star - abs_pattern);
        if (blen >= sizeof(base_dir)) return -1;
        memcpy(base_dir, abs_pattern, blen);
        base_dir[blen] = '\0';
        /* strip trailing slash */
        if (blen > 1 && base_dir[blen - 1] == '/') base_dir[blen - 1] = '\0';

        g_glob.paths = NULL; g_glob.count = 0; g_glob.cap = 0;
        g_glob.suffix_pat = star + 2; /* skip ** */
        nftw(base_dir, glob_collect_cb, 64, FTW_PHYS);
        srcs = g_glob.paths; nsrcs = g_glob.count;
    } else {
        glob_t gl; memset(&gl, 0, sizeof(gl));
        if (glob(abs_pattern, GLOB_BRACE | GLOB_TILDE, NULL, &gl) == 0) {
            srcs = malloc(gl.gl_pathc * sizeof(char *));
            if (srcs) {
                for (size_t i = 0; i < gl.gl_pathc; i++)
                    srcs[i] = strdup(gl.gl_pathv[i]);
                nsrcs = (int)gl.gl_pathc;
            }
        }
        globfree(&gl);
    }

    if (nsrcs == 0) {
        fprintf(stderr, "Warning: COPY pattern '%s' matched no files\n", src_pattern);
        free(srcs);
        return 0;
    }

    int ret = 0;
    for (int i = 0; i < nsrcs && ret == 0; i++) {
        char *src = srcs[i];
        /* strip trailing slash that glob may add for directories */
        size_t slen = strlen(src);
        if (slen > 1 && src[slen - 1] == '/') src[slen - 1] = '\0';

        /* Security: reject sources that escape the build context */
        char src_real[PATH_MAX];
        if (!realpath(src, src_real)) continue;
        if (strncmp(src_real, ctx_real, strlen(ctx_real)) != 0 ||
            (src_real[strlen(ctx_real)] != '/' && src_real[strlen(ctx_real)] != '\0')) {
            fprintf(stderr, "Error: COPY source '%s' escapes build context\n", src_pattern);
            ret = -1; break;
        }

        struct stat st;
        if (lstat(src, &st) != 0) continue;

        char dest_abs[PATH_MAX];
        if (S_ISDIR(st.st_mode)) {
            if (dest_is_dir) {
                const char *base = strrchr(src_real, '/');
                base = base ? base + 1 : src_real;
                snprintf(dest_abs, sizeof(dest_abs), "%s%s%s", temp_root, dest_in_root, base);
            } else {
                snprintf(dest_abs, sizeof(dest_abs), "%s%s", temp_root, dest_in_root);
            }
            mkdir_p(dest_abs, st.st_mode & 0777);
            copy_tree(src_real, dest_abs);
        } else {
            const char *base = strrchr(src_real, '/');
            base = base ? base + 1 : src_real;
            if (dest_is_dir || nsrcs > 1)
                snprintf(dest_abs, sizeof(dest_abs), "%s%s%s", temp_root, dest_in_root, base);
            else
                snprintf(dest_abs, sizeof(dest_abs), "%s%s", temp_root, dest_in_root);

            ensure_parent(dest_abs);

            if (S_ISLNK(st.st_mode)) {
                char target[PATH_MAX];
                ssize_t n = readlink(src, target, sizeof(target) - 1);
                if (n >= 0) { target[n] = '\0'; unlink(dest_abs); symlink(target, dest_abs); }
            } else {
                ret = copy_regular_file(src, dest_abs, st.st_mode & 0777);
            }
        }
    }

    for (int i = 0; i < nsrcs; i++) free(srcs[i]);
    free(srcs);
    return ret;
}
