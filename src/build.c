#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <limits.h>
#include <ftw.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <fnmatch.h>
#include <openssl/evp.h>
#include <cjson/cJSON.h>
#include "build.h"
#include "isolation.h"
#include "tar_utils.h"
#include "fsutil.h"
#include "cache.h"

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (buf) { fread(buf, 1, len, f); buf[len] = '\0'; }
    fclose(f); return buf;
}

static void sha256_str(const char *data, char out_hex[65]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, strlen(data));
    unsigned char md[32]; unsigned int mdlen;
    EVP_DigestFinal_ex(ctx, md, &mdlen);
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < 32; i++) sprintf(&out_hex[i * 2], "%02x", md[i]);
    out_hex[64] = '\0';
}

static double monotonic_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int sha256_file_hex(const char *path, char out_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { fclose(f); return -1; }
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        EVP_DigestUpdate(ctx, buf, n);
    if (ferror(f)) { EVP_MD_CTX_free(ctx); fclose(f); return -1; }
    unsigned char md[32]; unsigned int mdlen;
    EVP_DigestFinal_ex(ctx, md, &mdlen);
    EVP_MD_CTX_free(ctx);
    fclose(f);
    for (int i = 0; i < 32; i++) sprintf(&out_hex[i * 2], "%02x", md[i]);
    out_hex[64] = '\0';
    return 0;
}

/* ---- cache key ---- */
void compute_cache_key(char *out_hash, const char *prev, const char *ins,
                       const char *wd, const char *env, const char *copy_file_hashes) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, prev ? prev : "", prev ? strlen(prev) : 0);
    EVP_DigestUpdate(ctx, "\n", 1);
    EVP_DigestUpdate(ctx, ins ? ins : "", ins ? strlen(ins) : 0);
    EVP_DigestUpdate(ctx, "\n", 1);
    EVP_DigestUpdate(ctx, wd ? wd : "", wd ? strlen(wd) : 0);
    EVP_DigestUpdate(ctx, "\n", 1);
    EVP_DigestUpdate(ctx, env ? env : "", env ? strlen(env) : 0);
    EVP_DigestUpdate(ctx, "\n", 1);
    EVP_DigestUpdate(ctx, copy_file_hashes ? copy_file_hashes : "",
                     copy_file_hashes ? strlen(copy_file_hashes) : 0);
    unsigned char md[32]; unsigned int mdlen;
    EVP_DigestFinal_ex(ctx, md, &mdlen);
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < 32; i++) sprintf(&out_hash[i * 2], "%02x", md[i]);
    out_hash[64] = '\0';
}

/* ---- build-root cleanup ---- */
static int rm_entry(const char *path, const struct stat *sb, int typeflag,
                    struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

static void cleanup_build_root(const char *temp_root) {
    if (!temp_root || temp_root[0] == '\0') return;
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", temp_root);
    umount2(proc_path, MNT_DETACH);
    nftw(temp_root, rm_entry, 32, FTW_DEPTH | FTW_PHYS);
}

/* ---- in-memory environment accumulator ---- */
#define ENV_MAX 128
typedef struct {
    char *keys[ENV_MAX];
    char *vals[ENV_MAX];
    int   count;
} EnvMap;

static void env_upsert(EnvMap *em, const char *key, const char *val) {
    for (int i = 0; i < em->count; i++) {
        if (strcmp(em->keys[i], key) == 0) {
            free(em->vals[i]);
            em->vals[i] = strdup(val);
            return;
        }
    }
    if (em->count < ENV_MAX) {
        em->keys[em->count] = strdup(key);
        em->vals[em->count] = strdup(val);
        em->count++;
    }
}

static void env_free(EnvMap *em) {
    for (int i = 0; i < em->count; i++) { free(em->keys[i]); free(em->vals[i]); }
    em->count = 0;
}

/* NULL-terminated "KEY=VALUE" array. Free with env_envp_free(). */
static char **env_to_envp(const EnvMap *em) {
    char **envp = malloc((em->count + 1) * sizeof(char *));
    if (!envp) return NULL;
    for (int i = 0; i < em->count; i++) {
        size_t n = strlen(em->keys[i]) + 1 + strlen(em->vals[i]) + 1;
        char *kv = malloc(n);
        if (kv) snprintf(kv, n, "%s=%s", em->keys[i], em->vals[i]);
        envp[i] = kv;
    }
    envp[em->count] = NULL;
    return envp;
}

static void env_envp_free(char **envp) {
    if (!envp) return;
    for (int i = 0; envp[i]; i++) free(envp[i]);
    free(envp);
}

/* Keys sorted lexicographically: "k1=v1\nk2=v2\n..." (heap, caller frees) */
static char *env_canonical(const EnvMap *em) {
    if (em->count == 0) return strdup("");
    int order[ENV_MAX];
    for (int i = 0; i < em->count; i++) order[i] = i;
    for (int i = 1; i < em->count; i++) {        /* insertion sort */
        int tmp = order[i], j = i - 1;
        while (j >= 0 && strcmp(em->keys[order[j]], em->keys[tmp]) > 0)
            { order[j+1] = order[j]; j--; }
        order[j+1] = tmp;
    }
    size_t total = 0;
    for (int i = 0; i < em->count; i++)
        total += strlen(em->keys[order[i]]) + 1 + strlen(em->vals[order[i]]) + 2;
    char *out = malloc(total + 1);
    if (!out) return NULL;
    char *p = out;
    for (int i = 0; i < em->count; i++)
        p += sprintf(p, "%s=%s\n", em->keys[order[i]], em->vals[order[i]]);
    return out;
}

typedef struct {
    char *path; /* relative to root, no leading slash */
    struct stat st;
} fs_entry_t;

typedef struct {
    fs_entry_t *items;
    int count;
    int cap;
    const char *root;
} fs_walk_ctx_t;

typedef struct {
    char **paths;
    int count;
    int cap;
} str_list_t;

static fs_walk_ctx_t g_walk_ctx;

static int fs_add_entry(fs_walk_ctx_t *ctx, const char *rel, const struct stat *st) {
    if (ctx->count >= ctx->cap) {
        int newcap = ctx->cap ? ctx->cap * 2 : 256;
        fs_entry_t *tmp = realloc(ctx->items, (size_t)newcap * sizeof(fs_entry_t));
        if (!tmp) return -1;
        ctx->items = tmp;
        ctx->cap = newcap;
    }
    ctx->items[ctx->count].path = strdup(rel);
    if (!ctx->items[ctx->count].path) return -1;
    ctx->items[ctx->count].st = *st;
    ctx->count++;
    return 0;
}

static int walk_collect_cb(const char *fpath, const struct stat *sb,
                           int typeflag, struct FTW *ftwbuf) {
    (void)typeflag;
    if (!g_walk_ctx.root || !sb || !ftwbuf) return FTW_STOP;
    if (ftwbuf->level == 0) return 0;
    const char *rel = fpath + strlen(g_walk_ctx.root);
    if (*rel == '/') rel++;
    if (*rel == '\0') return 0;
    if (strcmp(rel, "proc") == 0 || strncmp(rel, "proc/", 5) == 0) return 0;
    if (fs_add_entry(&g_walk_ctx, rel, sb) != 0) return FTW_STOP;
    return 0;
}

static int cmp_fs_entry(const void *a, const void *b) {
    const fs_entry_t *ea = (const fs_entry_t *)a;
    const fs_entry_t *eb = (const fs_entry_t *)b;
    return strcmp(ea->path, eb->path);
}

static int snapshot_fs_state(const char *root, fs_entry_t **out, int *out_count) {
    g_walk_ctx.items = NULL;
    g_walk_ctx.count = 0;
    g_walk_ctx.cap = 0;
    g_walk_ctx.root = root;
    if (nftw(root, walk_collect_cb, 64, FTW_PHYS) != 0) {
        for (int i = 0; i < g_walk_ctx.count; i++) free(g_walk_ctx.items[i].path);
        free(g_walk_ctx.items);
        return -1;
    }
    qsort(g_walk_ctx.items, g_walk_ctx.count, sizeof(fs_entry_t), cmp_fs_entry);
    *out = g_walk_ctx.items;
    *out_count = g_walk_ctx.count;
    return 0;
}

static void free_fs_state(fs_entry_t *state, int count) {
    if (!state) return;
    for (int i = 0; i < count; i++) free(state[i].path);
    free(state);
}

static int find_fs_entry(const fs_entry_t *arr, int count, const char *path) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcmp(arr[mid].path, path);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int fs_meta_differs(const struct stat *a, const struct stat *b) {
    if ((a->st_mode & S_IFMT) != (b->st_mode & S_IFMT)) return 1;
    if ((a->st_mode & 07777) != (b->st_mode & 07777)) return 1;
    if (a->st_uid != b->st_uid || a->st_gid != b->st_gid) return 1;
    if (a->st_size != b->st_size) return 1;
#if defined(__linux__)
    if (a->st_mtim.tv_sec != b->st_mtim.tv_sec || a->st_mtim.tv_nsec != b->st_mtim.tv_nsec) return 1;
#else
    if (a->st_mtime != b->st_mtime) return 1;
#endif
    return 0;
}

static int str_list_add_owned(str_list_t *lst, char *owned) {
    if (!owned) return -1;
    if (lst->count >= lst->cap) {
        int newcap = lst->cap ? lst->cap * 2 : 256;
        char **tmp = realloc(lst->paths, (size_t)newcap * sizeof(char *));
        if (!tmp) { free(owned); return -1; }
        lst->paths = tmp;
        lst->cap = newcap;
    }
    lst->paths[lst->count++] = owned;
    return 0;
}

static int str_list_add_copy(str_list_t *lst, const char *s) {
    return str_list_add_owned(lst, strdup(s));
}

static int cmp_str_ptr(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcmp(sa, sb);
}

typedef struct {
    char **paths;
    int count;
    int cap;
    const char *suffix_pat;
} hash_glob_ctx_t;
static hash_glob_ctx_t g_hash_glob;

static int hash_glob_collect_cb(const char *fpath, const struct stat *sb,
                                int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag != FTW_F && typeflag != FTW_SL) return 0;
    const char *base = strrchr(fpath, '/');
    base = base ? base + 1 : fpath;
    const char *pat = g_hash_glob.suffix_pat;
    if (*pat == '/') pat++;
    if (fnmatch(pat, base, 0) != 0) return 0;
    if (g_hash_glob.count >= g_hash_glob.cap) {
        int newcap = g_hash_glob.cap ? g_hash_glob.cap * 2 : 64;
        char **tmp = realloc(g_hash_glob.paths, (size_t)newcap * sizeof(char *));
        if (!tmp) return FTW_STOP;
        g_hash_glob.paths = tmp;
        g_hash_glob.cap = newcap;
    }
    g_hash_glob.paths[g_hash_glob.count++] = strdup(fpath);
    return 0;
}

static void str_list_sort_unique(str_list_t *lst) {
    if (!lst || lst->count <= 1) return;
    qsort(lst->paths, lst->count, sizeof(char *), cmp_str_ptr);
    int out = 1;
    for (int i = 1; i < lst->count; i++) {
        if (strcmp(lst->paths[i], lst->paths[out - 1]) == 0) {
            free(lst->paths[i]);
            continue;
        }
        lst->paths[out++] = lst->paths[i];
    }
    lst->count = out;
}

static void str_list_free(str_list_t *lst) {
    if (!lst) return;
    for (int i = 0; i < lst->count; i++) free(lst->paths[i]);
    free(lst->paths);
    lst->paths = NULL;
    lst->count = 0;
    lst->cap = 0;
}

static int collect_copy_sources_for_cache(const char *context_dir, const char *src_pattern,
                                          str_list_t *sources) {
    char ctx_real[PATH_MAX];
    if (!realpath(context_dir, ctx_real)) return -1;

    char abs_pattern[PATH_MAX * 2];
    if (src_pattern[0] == '/')
        snprintf(abs_pattern, sizeof(abs_pattern), "%s", src_pattern);
    else
        snprintf(abs_pattern, sizeof(abs_pattern), "%s/%s", ctx_real, src_pattern);

    if (strstr(src_pattern, "**")) {
        char *star = strstr(abs_pattern, "**");
        char base_dir[PATH_MAX];
        size_t blen = (size_t)(star - abs_pattern);
        if (blen >= sizeof(base_dir)) return -1;
        memcpy(base_dir, abs_pattern, blen);
        base_dir[blen] = '\0';
        if (blen > 1 && base_dir[blen - 1] == '/') base_dir[blen - 1] = '\0';

        g_hash_glob.paths = NULL;
        g_hash_glob.count = 0;
        g_hash_glob.cap = 0;
        g_hash_glob.suffix_pat = star + 2;
        if (nftw(base_dir, hash_glob_collect_cb, 64, FTW_PHYS) != 0 &&
            errno != ENOENT) {
            for (int i = 0; i < g_hash_glob.count; i++) free(g_hash_glob.paths[i]);
            free(g_hash_glob.paths);
            return -1;
        }
        for (int i = 0; i < g_hash_glob.count; i++) {
            if (str_list_add_owned(sources, g_hash_glob.paths[i]) != 0) {
                for (int j = i; j < g_hash_glob.count; j++) free(g_hash_glob.paths[j]);
                free(g_hash_glob.paths);
                return -1;
            }
        }
        free(g_hash_glob.paths);
    } else {
        glob_t gl;
        memset(&gl, 0, sizeof(gl));
        int gr = glob(abs_pattern, GLOB_BRACE | GLOB_TILDE, NULL, &gl);
        if (gr != 0 && gr != GLOB_NOMATCH) {
            globfree(&gl);
            return -1;
        }
        for (size_t i = 0; i < gl.gl_pathc; i++) {
            if (str_list_add_copy(sources, gl.gl_pathv[i]) != 0) {
                globfree(&gl);
                return -1;
            }
        }
        globfree(&gl);
    }

    str_list_sort_unique(sources);
    return 0;
}

static char *build_copy_file_hashes(const char *context_dir, const char *src_pattern) {
    char ctx_real[PATH_MAX];
    if (!realpath(context_dir, ctx_real)) return NULL;

    str_list_t srcs = {0};
    if (collect_copy_sources_for_cache(context_dir, src_pattern, &srcs) != 0) {
        str_list_free(&srcs);
        return NULL;
    }

    size_t total = 1;
    char **reals = calloc((size_t)srcs.count, sizeof(char *));
    if (!reals) { str_list_free(&srcs); return NULL; }

    for (int i = 0; i < srcs.count; i++) {
        char src_real[PATH_MAX];
        if (!realpath(srcs.paths[i], src_real)) continue;
        size_t ctx_len = strlen(ctx_real);
        if (strncmp(src_real, ctx_real, ctx_len) != 0 ||
            (src_real[ctx_len] != '/' && src_real[ctx_len] != '\0')) {
            free(reals[i]);
            reals[i] = NULL;
            continue;
        }
        struct stat st;
        if (lstat(src_real, &st) != 0) continue;
        if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) continue;
        reals[i] = strdup(src_real);
        if (!reals[i]) continue;
        const char *rel = src_real + ctx_len;
        if (*rel == '/') rel++;
        total += strlen(rel) + 1 + 64 + 1;
    }

    char *out = malloc(total);
    if (!out) {
        for (int i = 0; i < srcs.count; i++) free(reals[i]);
        free(reals);
        str_list_free(&srcs);
        return NULL;
    }
    out[0] = '\0';

    size_t out_used = 0;
    for (int i = 0; i < srcs.count; i++) {
        if (!reals[i]) continue;
        const char *rel = reals[i] + strlen(ctx_real);
        if (*rel == '/') rel++;
        char h[65];
        if (sha256_file_hex(reals[i], h) == 0) {
            int n = snprintf(out + out_used, total - out_used, "%s\n%s\n", rel, h);
            if (n < 0 || (size_t)n >= total - out_used) {
                free(out);
                for (int j = 0; j < srcs.count; j++) free(reals[j]);
                free(reals);
                str_list_free(&srcs);
                return NULL;
            }
            out_used += (size_t)n;
        }
    }

    for (int i = 0; i < srcs.count; i++) free(reals[i]);
    free(reals);
    str_list_free(&srcs);
    return out;
}

static int touch_whiteout_file(const char *root, const char *rel_parent, const char *base_name,
                               char out_rel_path[PATH_MAX]) {
    char wh_name[NAME_MAX];
    snprintf(wh_name, sizeof(wh_name), ".wh.%s", base_name);

    char rel_path[PATH_MAX];
    int nrel;
    if (rel_parent[0] == '\0') nrel = snprintf(rel_path, sizeof(rel_path), "%s", wh_name);
    else nrel = snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_parent, wh_name);
    if (nrel < 0 || nrel >= (int)sizeof(rel_path)) return -1;

    char abs_path[PATH_MAX];
    int nabs = snprintf(abs_path, sizeof(abs_path), "%s/%s", root, rel_path);
    if (nabs < 0 || nabs >= (int)sizeof(abs_path)) return -1;
    char abs_parent[PATH_MAX];
    int npar = snprintf(abs_parent, sizeof(abs_parent), "%s", abs_path);
    if (npar < 0 || npar >= (int)sizeof(abs_parent)) return -1;
    char *slash = strrchr(abs_parent, '/');
    if (slash) { *slash = '\0'; if (mkdir_p(abs_parent, 0755) != 0) return -1; }

    int fd = open(abs_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    close(fd);
    snprintf(out_rel_path, PATH_MAX, "%s", rel_path);
    return 0;
}

static int has_deleted_ancestor(const str_list_t *deleted_dirs, const char *path) {
    for (int i = 0; i < deleted_dirs->count; i++) {
        size_t n = strlen(deleted_dirs->paths[i]);
        if (strncmp(path, deleted_dirs->paths[i], n) == 0 &&
            (path[n] == '/' || path[n] == '\0')) {
            return 1;
        }
    }
    return 0;
}

static int build_delta_paths(const char *root,
                             const fs_entry_t *pre, int pre_count,
                             const fs_entry_t *post, int post_count,
                             str_list_t *delta_paths,
                             str_list_t *temp_whiteouts) {
    int ret = 0;
    for (int i = 0; i < post_count; i++) {
        const fs_entry_t *cur = &post[i];
        int idx = find_fs_entry(pre, pre_count, cur->path);
        if (idx < 0) {
            if (str_list_add_copy(delta_paths, cur->path) != 0) return -1;
            continue;
        }
        if (fs_meta_differs(&pre[idx].st, &cur->st)) {
            if (str_list_add_copy(delta_paths, cur->path) != 0) return -1;
            continue;
        }
    }

    str_list_t deleted_dirs = {0};
    for (int i = 0; i < pre_count; i++) {
        if (find_fs_entry(post, post_count, pre[i].path) >= 0) continue;
        if (has_deleted_ancestor(&deleted_dirs, pre[i].path)) continue;
        const char *full = pre[i].path;
        const char *base = strrchr(full, '/');
        char parent[PATH_MAX];
        if (!base) {
            parent[0] = '\0';
            base = full;
        } else {
            size_t plen = (size_t)(base - full);
            if (plen >= sizeof(parent)) plen = sizeof(parent) - 1;
            memcpy(parent, full, plen);
            parent[plen] = '\0';
            base++;
        }
        char wh_rel[PATH_MAX];
        if (touch_whiteout_file(root, parent, base, wh_rel) != 0) { ret = -1; goto out; }
        if (str_list_add_copy(delta_paths, wh_rel) != 0) { ret = -1; goto out; }
        if (str_list_add_copy(temp_whiteouts, wh_rel) != 0) { ret = -1; goto out; }
        if (S_ISDIR(pre[i].st.st_mode) && str_list_add_copy(&deleted_dirs, pre[i].path) != 0) { ret = -1; goto out; }
    }
out:
    str_list_free(&deleted_dirs);
    if (ret != 0) return ret;

    str_list_sort_unique(delta_paths);
    str_list_sort_unique(temp_whiteouts);
    return 0;
}

static int resolve_workdir_path(const char *current_wd, const char *new_path,
                                char out[PATH_MAX]) {
    if (!new_path || !new_path[0]) return -1;

    char joined[PATH_MAX];
    if (new_path[0] == '/') {
        if (snprintf(joined, sizeof(joined), "%s", new_path) >= (int)sizeof(joined)) return -1;
    } else {
        const char *base = (current_wd && current_wd[0]) ? current_wd : "/";
        if (snprintf(joined, sizeof(joined), "%s/%s", base, new_path) >= (int)sizeof(joined)) return -1;
    }

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", joined) >= (int)sizeof(tmp)) return -1;

    char *parts[PATH_MAX / 2];
    int part_count = 0;
    char *save = NULL;
    char *tok = strtok_r(tmp, "/", &save);
    while (tok) {
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
            tok = strtok_r(NULL, "/", &save);
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            if (part_count == 0) return -1; /* would escape root */
            part_count--;
            tok = strtok_r(NULL, "/", &save);
            continue;
        }
        parts[part_count++] = tok;
        tok = strtok_r(NULL, "/", &save);
    }

    int off = 0;
    out[off++] = '/';
    out[off] = '\0';
    for (int i = 0; i < part_count; i++) {
        if (off >= PATH_MAX - 1) return -1;
        if (i > 0) out[off++] = '/';
        size_t plen = strlen(parts[i]);
        if (off + (int)plen >= PATH_MAX) return -1;
        memcpy(out + off, parts[i], plen);
        off += (int)plen;
        out[off] = '\0';
    }
    return 0;
}

static int ensure_workdir_exists(const char *temp_root, const char *wd) {
    if (!temp_root || !wd || wd[0] != '/') return -1;
    char abs_wd[PATH_MAX];
    if (snprintf(abs_wd, sizeof(abs_wd), "%s%s", temp_root, wd) >= (int)sizeof(abs_wd)) return -1;
    struct stat st;
    if (stat(abs_wd, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    if (errno != ENOENT) return -1;
    return mkdir_p(abs_wd, 0755);
}

/* ---- instruction memory management ---- */
static void free_instruction(instruction_t *instr) {
    free(instr->raw);
    free(instr->from_image);
    free(instr->from_tag);
    free(instr->copy_src);
    free(instr->copy_dest);
    free(instr->run_cmd);
    free(instr->workdir_path);
    free(instr->env_key);
    free(instr->env_value);
    if (instr->cmd_argv) {
        for (int i = 0; instr->cmd_argv[i]; i++) free(instr->cmd_argv[i]);
        free(instr->cmd_argv);
    }
}

void free_instructions(instruction_t *list, int count) {
    for (int i = 0; i < count; i++) free_instruction(&list[i]);
    free(list);
}

/* ---- parser ---- */
int parse_docksmithfile(const char *path, instruction_t **out_list, int *out_count) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }

    instruction_t *list = NULL;
    int count = 0, cap = 0, saw_from = 0, ret = 0;
    char line[1024];
    int line_no = 0;

    while (fgets(line, sizeof(line), f)) {
        line_no++;
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '#' || line[0] == '\0') continue;

        if (count >= cap) {
            int newcap = cap ? cap * 2 : 16;
            instruction_t *tmp = realloc(list, newcap * sizeof(instruction_t));
            if (!tmp) { ret = -1; goto done; }
            list = tmp; cap = newcap;
        }

        instruction_t *instr = &list[count];
        memset(instr, 0, sizeof(*instr));
        instr->line_no = line_no;
        instr->raw = strdup(line);
        if (!instr->raw) { ret = -1; goto done; }

        if (strncmp(line, "FROM ", 5) == 0) {
            if (saw_from || count > 0) {
                fprintf(stderr, "Error: FROM must be the first instruction (line %d)\n", line_no);
                free(instr->raw); instr->raw = NULL;
                ret = -1; goto done;
            }
            instr->kind = INSTR_FROM;
            saw_from = 1;
            char img_tag[256] = "";
            if (sscanf(line + 5, "%255s", img_tag) != 1) {
                fprintf(stderr, "Error: FROM requires an image name (line %d)\n", line_no);
                ret = -1; goto done;
            }
            char *colon = strchr(img_tag, ':');
            if (colon) {
                *colon = '\0';
                instr->from_image = strdup(img_tag);
                instr->from_tag   = strdup(colon + 1);
            } else {
                instr->from_image = strdup(img_tag);
                instr->from_tag   = strdup("latest");
            }

        } else if (strncmp(line, "COPY ", 5) == 0) {
            instr->kind = INSTR_COPY;
            char src[256] = "", dest[256] = "";
            int n = sscanf(line + 5, "%255s %255s", src, dest);
            if (n != 2) {
                fprintf(stderr, "Error: COPY requires exactly two arguments <src> <dest> (line %d)\n", line_no);
                ret = -1; goto done;
            }
            instr->copy_src  = strdup(src);
            instr->copy_dest = strdup(dest);

        } else if (strncmp(line, "RUN ", 4) == 0) {
            instr->kind    = INSTR_RUN;
            instr->run_cmd = strdup(line + 4);

        } else if (strncmp(line, "WORKDIR ", 8) == 0) {
            instr->kind = INSTR_WORKDIR;
            char wdpath[1024] = "";
            if (sscanf(line + 8, "%1023s", wdpath) != 1) {
                fprintf(stderr, "Error: WORKDIR requires a path (line %d)\n", line_no);
                ret = -1; goto done;
            }
            instr->workdir_path = strdup(wdpath);

        } else if (strncmp(line, "ENV ", 4) == 0) {
            instr->kind = INSTR_ENV;
            char *eq = strchr(line + 4, '=');
            if (!eq) {
                fprintf(stderr, "Error: ENV requires KEY=VALUE format (line %d)\n", line_no);
                ret = -1; goto done;
            }
            instr->env_key = strndup(line + 4, (size_t)(eq - (line + 4)));
            char *val = eq + 1;
            size_t vlen = strlen(val);
            if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"')
                instr->env_value = strndup(val + 1, vlen - 2);
            else
                instr->env_value = strdup(val);

        } else if (strncmp(line, "CMD", 3) == 0 &&
                   (line[3] == ' ' || line[3] == '\0')) {
            instr->kind = INSTR_CMD;
            const char *rest = line + 3;
            while (*rest == ' ') rest++;
            if (rest[0] != '[') {
                fprintf(stderr,
                    "Error: CMD must use JSON array form, e.g. CMD [\"prog\",\"arg\"] "
                    "(line %d). Shell form is not allowed per rubric §3.\n", line_no);
                ret = -1; goto done;
            }
            cJSON *arr = cJSON_Parse(rest);
            if (!arr || !cJSON_IsArray(arr)) {
                fprintf(stderr, "Error: CMD JSON array parse failed (line %d)\n", line_no);
                cJSON_Delete(arr);
                ret = -1; goto done;
            }
            int n = cJSON_GetArraySize(arr);
            instr->cmd_argv = malloc((n + 1) * sizeof(char *));
            if (!instr->cmd_argv) { cJSON_Delete(arr); ret = -1; goto done; }
            int ok = 1;
            for (int j = 0; j < n; j++) {
                cJSON *item = cJSON_GetArrayItem(arr, j);
                if (!cJSON_IsString(item)) {
                    fprintf(stderr, "Error: CMD array items must be strings (line %d)\n", line_no);
                    for (int k = 0; k < j; k++) free(instr->cmd_argv[k]);
                    free(instr->cmd_argv); instr->cmd_argv = NULL;
                    ok = 0; break;
                }
                instr->cmd_argv[j] = strdup(item->valuestring);
            }
            instr->cmd_argv[n] = NULL;
            cJSON_Delete(arr);
            if (!ok) { ret = -1; goto done; }

        } else {
            char kw[32] = "";
            sscanf(line, "%31s", kw);
            fprintf(stderr, "Error: unrecognized instruction '%s' at line %d\n", kw, line_no);
            free(instr->raw); instr->raw = NULL;
            ret = -1; goto done;
        }

        /* Enforce FROM-first for non-FROM instructions */
        if (!saw_from) {
            fprintf(stderr, "Error: first instruction must be FROM (line %d)\n", line_no);
            ret = -1; goto done;
        }

        count++;
    }

    if (!saw_from) {
        fprintf(stderr, "Error: Docksmithfile has no FROM instruction\n");
        ret = -1;
    }

done:
    fclose(f);
    if (ret != 0) {
        for (int i = 0; i < count; i++) free_instruction(&list[i]);
        free(list);
        return -1;
    }
    *out_list  = list;
    *out_count = count;
    return 0;
}

/* ---- build execution ---- */
int execute_build(const char *tag, const char *context, int use_cache) {
    if (geteuid() != 0) {
        fprintf(stderr, "Error: docksmith must be run as root. Try: sudo docksmith build ...\n");
        return 1;
    }

    char abs_ctx[PATH_MAX];
    if (!realpath(context, abs_ctx)) {
        fprintf(stderr, "Error: context directory '%s' not found\n", context);
        return 1;
    }

    /* Split tag "name:tag" -> img_name, img_tag */
    char img_name[256] = "", img_tag[256] = "latest";
    {
        const char *colon = strchr(tag, ':');
        if (colon) {
            size_t nlen = (size_t)(colon - tag);
            if (snprintf(img_name, sizeof(img_name), "%.*s",
                         (int)(nlen < sizeof(img_name) - 1 ? nlen : sizeof(img_name) - 1),
                         tag) < 0) {
                return 1;
            }
            if (snprintf(img_tag, sizeof(img_tag), "%s", colon + 1) >= (int)sizeof(img_tag)) {
                fprintf(stderr, "Error: image tag too long\n");
                return 1;
            }
        } else {
            if (snprintf(img_name, sizeof(img_name), "%s", tag) >= (int)sizeof(img_name)) {
                fprintf(stderr, "Error: image name too long\n");
                return 1;
            }
        }
    }

    char dfile[PATH_MAX + 20];
    snprintf(dfile, sizeof(dfile), "%s/Docksmithfile", abs_ctx);

    char mpath[PATH_MAX];
    snprintf(mpath, sizeof(mpath), "%s/.docksmith/images/%s_%s.json",
             getenv("HOME"), img_name, img_tag);
    bool manifest_existed_at_start = false;
    bool have_existing_created = false;
    char existing_created[64] = "";
    struct stat mst;
    if (stat(mpath, &mst) == 0) {
        manifest_existed_at_start = true;
        char *old_json = read_text_file(mpath);
        if (old_json) {
            cJSON *old_man = cJSON_Parse(old_json);
            if (old_man) {
                cJSON *old_created = cJSON_GetObjectItem(old_man, "created");
                if (old_created && cJSON_IsString(old_created) && old_created->valuestring[0]) {
                    snprintf(existing_created, sizeof(existing_created), "%s", old_created->valuestring);
                    have_existing_created = true;
                }
                cJSON_Delete(old_man);
            }
            free(old_json);
        }
    }

    instruction_t *instrs = NULL;
    int count = 0;
    if (parse_docksmithfile(dfile, &instrs, &count) != 0) return 1;

    char temp_root[] = "/tmp/docksmith_build_XXXXXX";
    if (!mkdtemp(temp_root)) {
        perror("mkdtemp");
        free_instructions(instrs, count);
        return 1;
    }
    chmod(temp_root, 0755);

    /* ISO-8601 UTC timestamp */
    char created_str[32];
    { time_t now = time(NULL); struct tm *u = gmtime(&now);
      strftime(created_str, sizeof(created_str), "%Y-%m-%dT%H:%M:%SZ", u); }

    /* key order: name, tag, digest, created, config, layers (Fix 4.1) */
    cJSON *man = cJSON_CreateObject();
    cJSON_AddStringToObject(man, "name",    img_name);
    cJSON_AddStringToObject(man, "tag",     img_tag);
    cJSON_AddStringToObject(man, "digest",  "");   /* filled at write time */
    cJSON_AddStringToObject(man, "created", created_str);
    /* config key order: Env, Cmd, WorkingDir (Fix 4.1) */
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddItemToObject(cfg, "Env", cJSON_CreateArray());
    cJSON_AddItemToObject(cfg, "Cmd", cJSON_CreateArray());
    cJSON_AddStringToObject(cfg, "WorkingDir", "/");
    cJSON_AddItemToObject(man, "config", cfg);
    cJSON_AddItemToObject(man, "layers", cJSON_CreateArray());

    char wd[PATH_MAX] = "/";
    bool wd_set = false;
    char prev[65] = "000";
    int ret = 0;
    EnvMap env_map; memset(&env_map, 0, sizeof(env_map));
    bool cascade_miss = false;
    double build_start = monotonic_seconds();

    for (int i = 0; i < count && ret == 0; i++) {
        instruction_t *instr = &instrs[i];

        switch (instr->kind) {

        case INSTR_FROM: {
            char *home = getenv("HOME");
            char base_mpath[PATH_MAX];
            snprintf(base_mpath, sizeof(base_mpath),
                     "%s/.docksmith/images/%s_%s.json",
                     home, instr->from_image, instr->from_tag);

            char *json = read_text_file(base_mpath);
            if (!json) {
                fprintf(stderr,
                    "Error: base image '%s:%s' not found in local store. "
                    "Run 'docksmith import -t %s:%s <rootfs.tar>' first.\n",
                    instr->from_image, instr->from_tag,
                    instr->from_image, instr->from_tag);
                ret = 1; break;
            }
            cJSON *base_man = cJSON_Parse(json);
            free(json);
            if (!base_man) {
                fprintf(stderr, "Error: corrupt manifest for '%s:%s'\n",
                        instr->from_image, instr->from_tag);
                ret = 1; break;
            }

            /* Use base manifest digest as initial prev for cache keys */
            cJSON *bdig = cJSON_GetObjectItem(base_man, "digest");
            if (bdig && cJSON_IsString(bdig)) {
                const char *d = bdig->valuestring;
                if (strncmp(d, "sha256:", 7) == 0) d += 7;
                strncpy(prev, d, 64); prev[64] = '\0';
            }

            /* Extract all base layers into temp_root */
            cJSON *base_layers = cJSON_GetObjectItem(base_man, "layers");
            cJSON *bl;
            cJSON_ArrayForEach(bl, base_layers) {
                cJSON *ditem = cJSON_GetObjectItem(bl, "digest");
                if (!ditem || !ditem->valuestring) continue;
                const char *d = ditem->valuestring;
                if (strncmp(d, "sha256:", 7) == 0) d += 7;
                char lpath[PATH_MAX];
                snprintf(lpath, sizeof(lpath), "%s/.docksmith/layers/%s.tar", home, d);
                if (extract_tar(lpath, temp_root) != 0) {
                    fprintf(stderr, "Error: failed to extract base layer %s\n",
                            ditem->valuestring);
                    cJSON_Delete(base_man); ret = 1; break;
                }
            }
            if (ret != 0) { cJSON_Delete(base_man); break; }

            /* Initialize our config and wd from base */
            cJSON *base_cfg = cJSON_GetObjectItem(base_man, "config");
            if (base_cfg) {
                cJSON *bwd = cJSON_GetObjectItem(base_cfg, "WorkingDir");
                if (bwd && cJSON_IsString(bwd)) {
                    wd_set = true;
                    if (bwd->valuestring[0])
                        snprintf(wd, sizeof(wd), "%s", bwd->valuestring);
                    else
                        snprintf(wd, sizeof(wd), "/");
                }
                cJSON_ReplaceItemInObject(cfg, "WorkingDir", cJSON_CreateString(wd));

                cJSON *benv = cJSON_GetObjectItem(base_cfg, "Env");
                if (benv) cJSON_ReplaceItemInObject(cfg, "Env", cJSON_Duplicate(benv, 1));

                cJSON *bcmd = cJSON_GetObjectItem(base_cfg, "Cmd");
                if (bcmd) cJSON_ReplaceItemInObject(cfg, "Cmd", cJSON_Duplicate(bcmd, 1));
            }

            /* Prepend base layers into our manifest (FROM is always first so list is empty) */
            cJSON *our_layers = cJSON_GetObjectItem(man, "layers");
            cJSON_ArrayForEach(bl, base_layers)
                cJSON_AddItemToArray(our_layers, cJSON_Duplicate(bl, 1));

            cJSON_Delete(base_man);
            printf("Step %d/%d : %s\n", i + 1, count, instr->raw);
            break;
        }

        case INSTR_WORKDIR:
            printf("Step %d/%d : %s\n", i + 1, count, instr->raw);
            if (resolve_workdir_path(wd, instr->workdir_path, wd) != 0) {
                fprintf(stderr, "Error: invalid WORKDIR '%s' (line %d)\n",
                        instr->workdir_path, instr->line_no);
                ret = 1;
                break;
            }
            cJSON_ReplaceItemInObject(cfg, "WorkingDir", cJSON_CreateString(wd));
            wd_set = true;
            break;

        case INSTR_ENV: {
            printf("Step %d/%d : %s\n", i + 1, count, instr->raw);
            /* Accumulate in both the in-memory map and manifest config.Env */
            env_upsert(&env_map, instr->env_key, instr->env_value);

            char kv[1024];
            snprintf(kv, sizeof(kv), "%s=%s", instr->env_key, instr->env_value);
            cJSON *env_arr = cJSON_GetObjectItem(cfg, "Env");
            int found = 0; cJSON *item; int idx = 0;
            cJSON_ArrayForEach(item, env_arr) {
                if (strncmp(item->valuestring, instr->env_key,
                            strlen(instr->env_key)) == 0 &&
                    item->valuestring[strlen(instr->env_key)] == '=') {
                    cJSON_ReplaceItemInArray(env_arr, idx, cJSON_CreateString(kv));
                    found = 1; break;
                }
                idx++;
            }
            if (!found) cJSON_AddItemToArray(env_arr, cJSON_CreateString(kv));
            break;
        }

        case INSTR_CMD: {
            printf("Step %d/%d : %s\n", i + 1, count, instr->raw);
            cJSON *cmd_arr = cJSON_CreateArray();
            for (int j = 0; instr->cmd_argv[j]; j++)
                cJSON_AddItemToArray(cmd_arr, cJSON_CreateString(instr->cmd_argv[j]));
            cJSON_ReplaceItemInObject(cfg, "Cmd", cmd_arr);
            break;
        }

        case INSTR_RUN:
        case INSTR_COPY: {
            if (ensure_workdir_exists(temp_root, wd) != 0) {
                fprintf(stderr, "Error: failed to prepare WORKDIR '%s'\n", wd);
                ret = 1;
                break;
            }

            char key[65];
            char *env_can = env_canonical(&env_map);
            const char *wd_for_key = wd_set ? wd : "";
            char *copy_hashes = NULL;
            if (instr->kind == INSTR_COPY) {
                copy_hashes = build_copy_file_hashes(abs_ctx, instr->copy_src);
                if (!copy_hashes) {
                    fprintf(stderr, "Error: failed to compute COPY source hashes\n");
                    free(env_can);
                    ret = 1;
                    break;
                }
            }
            compute_cache_key(key, prev, instr->raw, wd_for_key,
                              env_can ? env_can : "",
                              copy_hashes ? copy_hashes : "");
            free(env_can);
            free(copy_hashes);

            double step_start = monotonic_seconds();
            bool cache_hit = false;
            if (use_cache && !cascade_miss) {
                char *hit_digest = cache_lookup(key);
                if (hit_digest) {
                    cache_hit = true;
                    char layer_path[PATH_MAX];
                    snprintf(layer_path, sizeof(layer_path), "%s/.docksmith/layers/%s.tar",
                             getenv("HOME"), hit_digest);
                    struct stat layer_st;
                    long layer_size = (stat(layer_path, &layer_st) == 0) ? (long)layer_st.st_size : 0;

                    cJSON *layer = cJSON_CreateObject();
                    char dstr[80]; snprintf(dstr, sizeof(dstr), "sha256:%s", hit_digest);
                    cJSON_AddStringToObject(layer, "digest",    dstr);
                    cJSON_AddNumberToObject(layer, "size",      (double)layer_size);
                    cJSON_AddStringToObject(layer, "createdBy", instr->raw);
                    cJSON_AddItemToArray(cJSON_GetObjectItem(man, "layers"), layer);
                    snprintf(prev, sizeof(prev), "%s", hit_digest);

                    printf("Step %d/%d : %s [CACHE HIT]\n",
                           i + 1, count, instr->raw);
                    free(hit_digest);
                    break;
                }
            }
            if (use_cache && !cache_hit) cascade_miss = true;

            fs_entry_t *pre_state = NULL, *post_state = NULL;
            int pre_count = 0, post_count = 0;
            str_list_t delta = {0}, whiteouts = {0};
            if (snapshot_fs_state(temp_root, &pre_state, &pre_count) != 0) {
                fprintf(stderr, "Error: failed to snapshot pre-instruction filesystem\n");
                ret = 1;
                break;
            }

            if (instr->kind == INSTR_RUN) {
                char *args[] = {"/bin/sh", "-c", instr->run_cmd, NULL};
                char **envp = env_to_envp(&env_map);
                int r = execute_isolated(temp_root, args,
                                         env_map.count > 0 ? envp : NULL, wd);
                env_envp_free(envp);
                if (r != 0) {
                    free_fs_state(pre_state, pre_count);
                    ret = 1;
                    break;
                }
            } else {
                if (copy_into_rootfs(abs_ctx, instr->copy_src,
                                     instr->copy_dest, temp_root) != 0) {
                    free_fs_state(pre_state, pre_count);
                    ret = 1;
                    break;
                }
            }

            if (snapshot_fs_state(temp_root, &post_state, &post_count) != 0 ||
                build_delta_paths(temp_root, pre_state, pre_count, post_state, post_count,
                                  &delta, &whiteouts) != 0) {
                fprintf(stderr, "Error: failed to compute layer delta\n");
                free_fs_state(pre_state, pre_count);
                free_fs_state(post_state, post_count);
                str_list_free(&delta);
                str_list_free(&whiteouts);
                ret = 1;
                break;
            }

            char layer_digest[65];
            char tar_tmp[PATH_MAX];
            snprintf(tar_tmp, sizeof(tar_tmp), "%s/.docksmith/layers/%s.tmp.tar",
                     getenv("HOME"), key);

            path_set_t include = {.paths = delta.paths, .count = delta.count};
            if (create_delta_tar(tar_tmp, temp_root, &include) != 0 ||
                sha256_file_hex(tar_tmp, layer_digest) != 0) {
                fprintf(stderr, "Fatal: layer capture failed\n");
                ret = 1;
            }

            char tp[PATH_MAX];
            snprintf(tp, sizeof(tp), "%s/.docksmith/layers/%s.tar",
                     getenv("HOME"), layer_digest);
            if (ret == 0 && rename(tar_tmp, tp) != 0) {
                if (errno == EXDEV) {
                    FILE *in = fopen(tar_tmp, "rb");
                    FILE *out = fopen(tp, "wb");
                    if (!in || !out) ret = 1;
                    else {
                        char b[65536];
                        size_t n;
                        while ((n = fread(b, 1, sizeof(b), in)) > 0)
                            if (fwrite(b, 1, n, out) != n) { ret = 1; break; }
                        if (ferror(in)) ret = 1;
                    }
                    if (in) fclose(in);
                    if (out) fclose(out);
                    unlink(tar_tmp);
                } else {
                    ret = 1;
                }
            }
            if (ret != 0) unlink(tar_tmp);
            if (ret == 0 && use_cache) cache_store(key, layer_digest);
            snprintf(prev, sizeof(prev), "%s", layer_digest);

            for (int wi = 0; wi < whiteouts.count; wi++) {
                char wh_abs[PATH_MAX];
                snprintf(wh_abs, sizeof(wh_abs), "%s/%s", temp_root, whiteouts.paths[wi]);
                unlink(wh_abs);
            }
            free_fs_state(pre_state, pre_count);
            free_fs_state(post_state, post_count);
            str_list_free(&delta);
            str_list_free(&whiteouts);
            if (ret != 0) break;

            /* layer key order: digest, size, createdBy (Fix 4.2) */
            struct stat layer_st;
            long layer_size = (stat(tp, &layer_st) == 0) ? (long)layer_st.st_size : 0;
            cJSON *layer = cJSON_CreateObject();
            char dstr[80]; snprintf(dstr, sizeof(dstr), "sha256:%s", layer_digest);
            cJSON_AddStringToObject(layer, "digest",    dstr);
            cJSON_AddNumberToObject(layer, "size",      (double)layer_size);
            cJSON_AddStringToObject(layer, "createdBy", instr->raw);
            cJSON_AddItemToArray(cJSON_GetObjectItem(man, "layers"), layer);
            printf("Step %d/%d : %s [CACHE MISS] %.2fs\n",
                   i + 1, count, instr->raw,
                   monotonic_seconds() - step_start);
            break;
        }

        } /* switch */
    }

    if (ret == 0) {
        if (!cascade_miss && manifest_existed_at_start && have_existing_created)
            cJSON_ReplaceItemInObject(man, "created", cJSON_CreateString(existing_created));

        /* Fix 4.1: compute manifest digest with "digest":"", then set and reserialize */
        char *tmp = cJSON_PrintUnformatted(man);
        if (tmp) {
            char mdig[65]; sha256_str(tmp, mdig); free(tmp);
            char mdstr[72]; snprintf(mdstr, sizeof(mdstr), "sha256:%s", mdig);
            cJSON_SetValuestring(cJSON_GetObjectItem(man, "digest"), mdstr);
        }
        char *final_json = cJSON_PrintUnformatted(man);
        FILE *mf = fopen(mpath, "w");
        if (!mf || !final_json) {
            fprintf(stderr, "Error: cannot write manifest\n");
            free(final_json); ret = 1;
        } else {
            fprintf(mf, "%s\n", final_json);
            fclose(mf); free(final_json);
            const char *dig = cJSON_GetObjectItem(man, "digest")->valuestring;
            if (strncmp(dig, "sha256:", 7) == 0) dig += 7;
            printf("Successfully built sha256:%.12s %s (%.2fs)\n",
                   dig, tag, monotonic_seconds() - build_start);
        }
    }

    env_free(&env_map);
    free_instructions(instrs, count);
    cJSON_Delete(man);
    cleanup_build_root(temp_root);
    return ret;
}
