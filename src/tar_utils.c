#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <ftw.h>
#include <limits.h>
#include <errno.h>
#include "tar_utils.h"

int extract_tar(const char *tar_filename, const char *dest_dir) {
    struct archive *a = archive_read_new();
    struct archive *ext = archive_write_disk_new();
    struct archive_entry *entry;
    int ret = 0;
    char cwd[PATH_MAX];
    int cwd_ok = 0;
    if (!a || !ext) {
        if (a) archive_read_free(a);
        if (ext) archive_write_free(ext);
        return 1;
    }
    archive_read_support_format_tar(a);
    archive_read_support_filter_all(a);
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);

    if (archive_read_open_filename(a, tar_filename, 10240) != ARCHIVE_OK) { ret = 1; goto out; }
    if (getcwd(cwd, sizeof(cwd))) cwd_ok = 1;
    if (chdir(dest_dir) != 0) { ret = 1; goto out; }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (archive_write_header(ext, entry) != ARCHIVE_OK) { ret = 1; break; }
        if (archive_entry_size(entry) > 0) {
            const void *buff; size_t size; int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                if (archive_write_data_block(ext, buff, size, offset) != ARCHIVE_OK) {
                    ret = 1; break;
                }
            }
            if (ret != 0) break;
        }
        if (archive_write_finish_entry(ext) != ARCHIVE_OK) { ret = 1; break; }
    }
out:
    archive_read_close(a); archive_read_free(a);
    archive_write_close(ext); archive_write_free(ext);
    if (cwd_ok) chdir(cwd);
    return ret;
}

/* ---- deterministic tar via libarchive ---- */

typedef struct {
    char **paths;
    int    count;
    int    cap;
} PathList;

static PathList g_paths;

static int collect_cb(const char *fpath, const struct stat *sb,
                      int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    if (g_paths.count >= g_paths.cap) {
        int newcap = g_paths.cap ? g_paths.cap * 2 : 256;
        char **tmp = realloc(g_paths.paths, newcap * sizeof(char *));
        if (!tmp) return FTW_STOP;
        g_paths.paths = tmp;
        g_paths.cap   = newcap;
    }
    char *copy = strdup(fpath);
    if (!copy) return FTW_STOP;
    g_paths.paths[g_paths.count++] = copy;
    return 0;
}

static int cmp_path(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static int write_paths_tar(const char *tar_filename, const char *src_dir,
                           char **rel_paths, int count) {
    struct archive *a = archive_write_new();
    /* pax_restricted: PAX extended headers only when necessary */
    archive_write_set_format_pax_restricted(a);
    if (archive_write_open_filename(a, tar_filename) != ARCHIVE_OK) {
        fprintf(stderr, "archive_write_open: %s\n", archive_error_string(a));
        archive_write_free(a);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        const char *rel = rel_paths[i];
        char abs[PATH_MAX];
        if (strcmp(rel, ".") == 0 || rel[0] == '\0')
            snprintf(abs, sizeof(abs), "%s", src_dir);
        else
            snprintf(abs, sizeof(abs), "%s/%s", src_dir, rel);

        struct stat st;
        if (lstat(abs, &st) != 0) {
            if (errno == ENOENT) continue;
            archive_write_close(a);
            archive_write_free(a);
            return 1;
        }

        struct archive_entry *entry = archive_entry_new();
        if (!entry) {
            archive_write_close(a);
            archive_write_free(a);
            return 1;
        }

        char entry_path[PATH_MAX + 3];
        if (strcmp(rel, ".") == 0 || rel[0] == '\0')
            snprintf(entry_path, sizeof(entry_path), ".");
        else
            snprintf(entry_path, sizeof(entry_path), "./%s", rel);
        archive_entry_set_pathname(entry, entry_path);

        /* Copy stat, then zero all timestamps and ownership for determinism */
        archive_entry_copy_stat(entry, &st);
        archive_entry_set_mtime(entry, 0, 0);
        archive_entry_unset_atime(entry);
        archive_entry_unset_ctime(entry);
        archive_entry_set_uid(entry, 0);
        archive_entry_set_gid(entry, 0);
        archive_entry_set_uname(entry, "root");
        archive_entry_set_gname(entry, "root");

        if (S_ISLNK(st.st_mode)) {
            char link_target[PATH_MAX];
            ssize_t n = readlink(abs, link_target, sizeof(link_target) - 1);
            if (n >= 0) { link_target[n] = '\0'; archive_entry_set_symlink(entry, link_target); }
        }

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            archive_entry_free(entry);
            archive_write_close(a);
            archive_write_free(a);
            return 1;
        }

        if (S_ISREG(st.st_mode) && st.st_size > 0) {
            FILE *fp = fopen(abs, "rb");
            if (fp) {
                char buf[65536]; size_t n;
                while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
                    archive_write_data(a, buf, n);
                fclose(fp);
            }
        }

        archive_entry_free(entry);
    }

    archive_write_close(a);
    archive_write_free(a);
    return 0;
}

int create_layer_tar(const char *tar_filename, const char *src_dir) {
    g_paths.paths = NULL; g_paths.count = 0; g_paths.cap = 0;
    if (nftw(src_dir, collect_cb, 64, FTW_PHYS) != 0) {
        for (int i = 0; i < g_paths.count; i++) free(g_paths.paths[i]);
        free(g_paths.paths);
        return 1;
    }

    qsort(g_paths.paths, g_paths.count, sizeof(char *), cmp_path);
    size_t src_len = strlen(src_dir);

    char **rels = malloc((g_paths.count + 1) * sizeof(char *));
    if (!rels) {
        for (int i = 0; i < g_paths.count; i++) free(g_paths.paths[i]);
        free(g_paths.paths);
        return 1;
    }

    int rcount = 0;
    for (int i = 0; i < g_paths.count; i++) {
        const char *abs = g_paths.paths[i];
        const char *rel = abs + src_len;
        if (*rel == '/') rel++;
        if (*rel == '\0') rels[rcount++] = strdup(".");
        else rels[rcount++] = strdup(rel);
        if (!rels[rcount - 1]) {
            for (int j = 0; j < rcount - 1; j++) free(rels[j]);
            free(rels);
            for (int j = 0; j < g_paths.count; j++) free(g_paths.paths[j]);
            free(g_paths.paths);
            return 1;
        }
    }

    int ret = write_paths_tar(tar_filename, src_dir, rels, rcount);
    for (int i = 0; i < rcount; i++) free(rels[i]);
    free(rels);

    for (int i = 0; i < g_paths.count; i++) free(g_paths.paths[i]);
    free(g_paths.paths);
    return ret;
}

int create_delta_tar(const char *tar_filename, const char *src_dir,
                     const path_set_t *paths_to_include) {
    if (!paths_to_include || paths_to_include->count < 0) return 1;
    if (paths_to_include->count == 0) {
        struct archive *a = archive_write_new();
        archive_write_set_format_pax_restricted(a);
        if (archive_write_open_filename(a, tar_filename) != ARCHIVE_OK) {
            archive_write_free(a);
            return 1;
        }
        archive_write_close(a);
        archive_write_free(a);
        return 0;
    }

    char **sorted = malloc((size_t)paths_to_include->count * sizeof(char *));
    if (!sorted) return 1;
    for (int i = 0; i < paths_to_include->count; i++) sorted[i] = paths_to_include->paths[i];
    qsort(sorted, paths_to_include->count, sizeof(char *), cmp_path);
    int ret = write_paths_tar(tar_filename, src_dir, sorted, paths_to_include->count);
    free(sorted);
    return ret;
}
