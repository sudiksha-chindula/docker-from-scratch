#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <ftw.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <openssl/evp.h>
#include <cjson/cJSON.h>
#include "cli.h"
#include "build.h"
#include "isolation.h"
#include "tar_utils.h"

/* ---- shared helpers ---- */

static char *read_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long length = ftell(f);
    if (length < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *data = malloc(length + 1);
    if (data) {
        size_t got = fread(data, 1, (size_t)length, f);
        if (got != (size_t)length || ferror(f)) { free(data); data = NULL; }
        else data[length] = '\0';
    }
    fclose(f);
    return data;
}

static int sha256_file(const char *path, char out_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { fclose(f); return -1; }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx); fclose(f); return -1;
    }
    unsigned char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (EVP_DigestUpdate(ctx, buf, n) != 1) {
            EVP_MD_CTX_free(ctx); fclose(f); return -1;
        }
    }
    if (ferror(f)) { EVP_MD_CTX_free(ctx); fclose(f); return -1; }
    fclose(f);
    unsigned char md[32]; unsigned int mdlen;
    if (EVP_DigestFinal_ex(ctx, md, &mdlen) != 1 || mdlen != 32) {
        EVP_MD_CTX_free(ctx); return -1;
    }
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < 32; i++) sprintf(&out_hex[i * 2], "%02x", md[i]);
    out_hex[64] = '\0';
    return 0;
}

static void sha256_str(const char *data, char out_hex[65]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { out_hex[0] = '\0'; return; }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, strlen(data)) != 1) {
        EVP_MD_CTX_free(ctx); out_hex[0] = '\0'; return;
    }
    unsigned char md[32]; unsigned int mdlen;
    if (EVP_DigestFinal_ex(ctx, md, &mdlen) != 1 || mdlen != 32) {
        EVP_MD_CTX_free(ctx); out_hex[0] = '\0'; return;
    }
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < 32; i++) sprintf(&out_hex[i * 2], "%02x", md[i]);
    out_hex[64] = '\0';
}

/* "name:tag" or "name" -> ~/.docksmith/images/name_tag.json */
static void manifest_path_for(const char *image, char *out, size_t len) {
    char *home = getenv("HOME");
    if (!home || !image || !out || len == 0) return;
    const char *colon = strchr(image, ':');
    if (colon) {
        char nm[256] = "";
        size_t nlen = (size_t)(colon - image);
        if (snprintf(nm, sizeof(nm), "%.*s",
                     (int)(nlen < sizeof(nm) - 1 ? nlen : sizeof(nm) - 1),
                     image) < 0) return;
        snprintf(out, len, "%s/.docksmith/images/%s_%s.json", home, nm, colon + 1);
    } else {
        snprintf(out, len, "%s/.docksmith/images/%s_latest.json", home, image);
    }
}

static int copy_file_bytes(const char *src, const char *dest) {
    int fd_in = open(src, O_RDONLY);
    if (fd_in < 0) return -1;
    int fd_out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) { close(fd_in); return -1; }
    char buf[65536]; ssize_t n; int ret = 0;
    while ((n = read(fd_in, buf, sizeof(buf))) > 0)
        if (write(fd_out, buf, n) != n) { ret = -1; break; }
    close(fd_in); close(fd_out);
    return ret;
}

static int rm_entry_cli(const char *path, const struct stat *sb,
                        int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

typedef struct {
    char name[128];
    char tag[128];
    char id[13];
    char created_display[20];
    time_t created_ts;
    int has_created;
} image_row_t;

static int image_row_cmp(const void *a, const void *b) {
    const image_row_t *ra = (const image_row_t *)a;
    const image_row_t *rb = (const image_row_t *)b;
    if (ra->has_created != rb->has_created) return rb->has_created - ra->has_created;
    if (ra->has_created && rb->has_created) {
        if (ra->created_ts < rb->created_ts) return 1;
        if (ra->created_ts > rb->created_ts) return -1;
    }
    int nc = strcmp(ra->name, rb->name);
    if (nc != 0) return nc;
    return strcmp(ra->tag, rb->tag);
}

static void cleanup_run_root(const char *temp_root) {
    if (!temp_root || temp_root[0] == '\0') return;
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", temp_root);
    umount2(proc_path, MNT_DETACH);
    nftw(temp_root, rm_entry_cli, 32, FTW_DEPTH | FTW_PHYS);
}

/* ---- handle_import ---- */

int handle_import(int argc, char **argv) {
    if (geteuid() != 0) {
        fprintf(stderr, "Error: docksmith must be run as root. Try: sudo docksmith import ...\n");
        return 1;
    }
    char *tag_arg = NULL, *tarfile = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) tag_arg = argv[++i];
        else if (!tarfile) tarfile = argv[i];
        else { fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]); return 1; }
    }
    if (!tag_arg) { fprintf(stderr, "Error: -t <name:tag> is required\n"); return 1; }
    if (!tarfile) { fprintf(stderr, "Error: <rootfs-tar> is required\n"); return 1; }

    char img_name[256] = "", img_tag[256] = "latest";
    const char *colon = strchr(tag_arg, ':');
    if (colon) {
        size_t nlen = (size_t)(colon - tag_arg);
        if (snprintf(img_name, sizeof(img_name), "%.*s",
                     (int)(nlen < sizeof(img_name) - 1 ? nlen : sizeof(img_name) - 1),
                     tag_arg) < 0) return 1;
        if (snprintf(img_tag, sizeof(img_tag), "%s", colon + 1) >= (int)sizeof(img_tag)) {
            fprintf(stderr, "Error: tag too long\n");
            return 1;
        }
    } else {
        if (snprintf(img_name, sizeof(img_name), "%s", tag_arg) >= (int)sizeof(img_name)) {
            fprintf(stderr, "Error: name too long\n");
            return 1;
        }
    }

    char layer_digest[65];
    if (sha256_file(tarfile, layer_digest) != 0) {
        fprintf(stderr, "Error: cannot read '%s': %s\n", tarfile, strerror(errno));
        return 1;
    }

    char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME is not set\n");
        return 1;
    }
    char layer_path[PATH_MAX];
    snprintf(layer_path, sizeof(layer_path), "%s/.docksmith/layers/%s.tar",
             home, layer_digest);
    if (copy_file_bytes(tarfile, layer_path) != 0) {
        fprintf(stderr, "Error: failed to copy tarball to layer store\n");
        return 1;
    }

    struct stat tst;
    if (stat(tarfile, &tst) != 0) {
        fprintf(stderr, "Error: cannot stat '%s': %s\n", tarfile, strerror(errno));
        return 1;
    }

    time_t now = time(NULL);
    struct tm *tm_utc = gmtime(&now);
    char created[32];
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", tm_utc);

    /* Build manifest — key order: name,tag,digest,created,config,layers */
    cJSON *man = cJSON_CreateObject();
    if (!man) return 1;
    cJSON_AddStringToObject(man, "name",    img_name);
    cJSON_AddStringToObject(man, "tag",     img_tag);
    cJSON_AddStringToObject(man, "digest",  "");  /* placeholder */
    cJSON_AddStringToObject(man, "created", created);

    cJSON *cfg = cJSON_CreateObject();
    if (!cfg) { cJSON_Delete(man); return 1; }
    cJSON *env_arr = cJSON_CreateArray();
    cJSON *cmd_arr = cJSON_CreateArray();
    if (!env_arr || !cmd_arr) {
        cJSON_Delete(env_arr); cJSON_Delete(cmd_arr);
        cJSON_Delete(cfg); cJSON_Delete(man); return 1;
    }
    cJSON_AddItemToObject(cfg, "Env", env_arr);
    cJSON_AddItemToObject(cfg, "Cmd", cmd_arr);
    cJSON_AddStringToObject(cfg, "WorkingDir", "/");
    cJSON_AddItemToObject(man, "config", cfg);

    cJSON *lentry = cJSON_CreateObject();
    if (!lentry) { cJSON_Delete(man); return 1; }
    char ldig[72]; snprintf(ldig, sizeof(ldig), "sha256:%s", layer_digest);
    cJSON_AddStringToObject(lentry, "digest", ldig);
    cJSON_AddNumberToObject(lentry, "size",   (double)tst.st_size);
    char createdby[PATH_MAX + 32];
    snprintf(createdby, sizeof(createdby), "imported from %s", tarfile);
    cJSON_AddStringToObject(lentry, "createdBy", createdby);
    cJSON *layers = cJSON_CreateArray();
    if (!layers) { cJSON_Delete(lentry); cJSON_Delete(man); return 1; }
    cJSON_AddItemToArray(layers, lentry);
    cJSON_AddItemToObject(man, "layers", layers);

    /* Digest = sha256 of unformatted JSON with digest="" */
    char *tmp_json = cJSON_PrintUnformatted(man);
    if (!tmp_json) { cJSON_Delete(man); return 1; }
    char man_digest[65]; sha256_str(tmp_json, man_digest); free(tmp_json);
    if (man_digest[0] == '\0') { cJSON_Delete(man); return 1; }
    char man_dstr[72]; snprintf(man_dstr, sizeof(man_dstr), "sha256:%s", man_digest);
    cJSON *dig_item = cJSON_GetObjectItem(man, "digest");
    if (!dig_item || !cJSON_SetValuestring(dig_item, man_dstr)) { cJSON_Delete(man); return 1; }

    char *final_json = cJSON_PrintUnformatted(man);
    cJSON_Delete(man);
    if (!final_json) return 1;

    char mpath[PATH_MAX];
    snprintf(mpath, sizeof(mpath), "%s/.docksmith/images/%s_%s.json",
             home, img_name, img_tag);
    FILE *mf = fopen(mpath, "w");
    if (!mf) { perror(mpath); free(final_json); return 1; }
    if (fprintf(mf, "%s\n", final_json) < 0) { fclose(mf); free(final_json); return 1; }
    fclose(mf); free(final_json);

    printf("Imported %s:%s\n  layer:    sha256:%.12s\n  manifest: %s\n",
           img_name, img_tag, layer_digest, mpath);
    return 0;
}

/* ---- handle_build ---- */

int handle_build(int argc, char **argv) {
    char *tag = NULL, *context = NULL;
    int use_cache = 1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)  { tag = argv[++i]; }
        else if (strcmp(argv[i], "--no-cache") == 0)      { use_cache = 0; }
        else if (!context)                                 { context = argv[i]; }
        else {
            fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]);
            return 1;
        }
    }
    if (!tag)     { fprintf(stderr, "Error: -t <name:tag> is required\n"); return 1; }
    if (!context) { fprintf(stderr, "Error: context directory is required\n"); return 1; }
    return execute_build(tag, context, use_cache);
}

/* ---- handle_images ---- */

int handle_images(int argc, char **argv) {
    (void)argc; (void)argv;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.docksmith/images/", getenv("HOME"));
    DIR *dir = opendir(path);
    if (!dir) { printf("No images found.\n"); return 0; }

    image_row_t *rows = NULL;
    int row_count = 0, row_cap = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strstr(ent->d_name, ".json")) continue;
        char filepath[PATH_MAX * 2];
        snprintf(filepath, sizeof(filepath), "%s%s", path, ent->d_name);
        char *json_data = read_file(filepath);
        if (!json_data) continue;
        cJSON *manifest = cJSON_Parse(json_data);
        free(json_data);
        if (!manifest) continue;

        cJSON *name    = cJSON_GetObjectItem(manifest, "name");
        cJSON *tag     = cJSON_GetObjectItem(manifest, "tag");
        cJSON *digest  = cJSON_GetObjectItem(manifest, "digest");
        cJSON *created = cJSON_GetObjectItem(manifest, "created");

        const char *id = (digest && cJSON_IsString(digest) && digest->valuestring)
                         ? digest->valuestring : "unknown";
        if (strncmp(id, "sha256:", 7) == 0) id += 7;  /* strip prefix */

        if (row_count >= row_cap) {
            int newcap = row_cap ? row_cap * 2 : 16;
            image_row_t *tmp = realloc(rows, (size_t)newcap * sizeof(*rows));
            if (!tmp) { cJSON_Delete(manifest); break; }
            rows = tmp; row_cap = newcap;
        }
        image_row_t *row = &rows[row_count];
        memset(row, 0, sizeof(*row));

        snprintf(row->name, sizeof(row->name), "%s",
                 (name && cJSON_IsString(name) && name->valuestring) ? name->valuestring : "none");
        snprintf(row->tag, sizeof(row->tag), "%s",
                 (tag && cJSON_IsString(tag) && tag->valuestring) ? tag->valuestring : "latest");
        snprintf(row->id, sizeof(row->id), "%.12s", id);

        row->has_created = 0;
        snprintf(row->created_display, sizeof(row->created_display), "unknown");
        if (created && cJSON_IsString(created) && created->valuestring) {
            struct tm utc_tm;
            memset(&utc_tm, 0, sizeof(utc_tm));
            if (strptime(created->valuestring, "%Y-%m-%dT%H:%M:%SZ", &utc_tm)) {
                time_t ts = timegm(&utc_tm);
                if (ts != (time_t)-1) {
                    row->created_ts = ts;
                    row->has_created = 1;
                    struct tm local_tm;
                    if (localtime_r(&ts, &local_tm))
                        strftime(row->created_display, sizeof(row->created_display),
                                 "%Y-%m-%d %H:%M:%S", &local_tm);
                }
            }
        }
        row_count++;

        cJSON_Delete(manifest);
    }
    closedir(dir);

    qsort(rows, (size_t)row_count, sizeof(*rows), image_row_cmp);
    printf("%-20s %-10s %-14s %s\n", "NAME", "TAG", "IMAGE ID", "CREATED");
    for (int i = 0; i < row_count; i++) {
        printf("%-20s %-10s %-14s %s\n",
               rows[i].name, rows[i].tag, rows[i].id, rows[i].created_display);
    }
    free(rows);
    return 0;
}

/* ---- handle_rmi ---- */

int handle_rmi(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: docksmith rmi <name:tag>\n");
        return 1;
    }
    char mpath[PATH_MAX];
    manifest_path_for(argv[2], mpath, sizeof(mpath));

    char *json_data = read_file(mpath);
    if (!json_data) {
        fprintf(stderr, "Error: image '%s' not found\n", argv[2]);
        return 1;
    }

    cJSON *manifest = cJSON_Parse(json_data);
    free(json_data);
    if (!manifest) {
        fprintf(stderr, "Error: failed to parse manifest for '%s'\n", argv[2]);
        return 1;
    }

    const char *home = getenv("HOME");
    if (!home) {
        cJSON_Delete(manifest);
        fprintf(stderr, "Error: HOME is not set\n");
        return 1;
    }

    char *deleted[1024] = {0};
    int deleted_count = 0;
    cJSON *layers = cJSON_GetObjectItem(manifest, "layers");
    cJSON *layer;
    cJSON_ArrayForEach(layer, layers) {
        cJSON *ditem = cJSON_GetObjectItem(layer, "digest");
        if (!ditem || !cJSON_IsString(ditem)) continue;
        const char *dig = ditem->valuestring;
        if (strncmp(dig, "sha256:", 7) == 0) dig += 7;
        int seen = 0;
        for (int i = 0; i < deleted_count; i++)
            if (strcmp(deleted[i], dig) == 0) { seen = 1; break; }
        if (seen) continue;

        char lpath[PATH_MAX];
        snprintf(lpath, sizeof(lpath), "%s/.docksmith/layers/%s.tar", home, dig);
        if (unlink(lpath) == 0 || errno == ENOENT) {
            if (deleted_count < (int)(sizeof(deleted) / sizeof(deleted[0]))) {
                deleted[deleted_count] = strdup(dig);
                if (deleted[deleted_count]) deleted_count++;
            }
        }
    }

    if (remove(mpath) != 0) {
        for (int i = 0; i < deleted_count; i++) free(deleted[i]);
        cJSON_Delete(manifest);
        fprintf(stderr, "Error: failed to remove manifest for '%s'\n", argv[2]);
        return 1;
    }

    char cache_dir[PATH_MAX];
    snprintf(cache_dir, sizeof(cache_dir), "%s/.docksmith/cache", home);
    DIR *dir = opendir(cache_dir);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char cpath[PATH_MAX];
            int n = snprintf(cpath, sizeof(cpath), "%s/%s", cache_dir, ent->d_name);
            if (n < 0 || n >= (int)sizeof(cpath)) continue;
            FILE *cf = fopen(cpath, "r");
            if (!cf) continue;
            char line[256];
            if (!fgets(line, sizeof(line), cf)) { fclose(cf); continue; }
            fclose(cf);
            line[strcspn(line, "\r\n")] = '\0';
            for (int i = 0; i < deleted_count; i++) {
                if (strcmp(line, deleted[i]) == 0) {
                    unlink(cpath);
                    break;
                }
            }
        }
        closedir(dir);
    }

    for (int i = 0; i < deleted_count; i++) free(deleted[i]);
    cJSON_Delete(manifest);
    printf("Untagged: %s\n", argv[2]);
    return 0;
}

/* ---- handle_run ---- */

int handle_run(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: docksmith run [-e KEY=VAL] <image:tag> [cmd args...]\n");
        return 1;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "Error: docksmith must be run as root. Try: sudo docksmith run ...\n");
        return 1;
    }

    char *image_name = NULL;
    char *env_overrides[64] = {NULL};
    int env_count = 0;
    int cmd_start = -1;

    /* -e flags come before image; everything after image is the command */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            env_overrides[env_count++] = argv[++i];
        } else if (!image_name) {
            image_name = argv[i];
            cmd_start = i + 1;
            break;
        }
    }
    if (!image_name) { fprintf(stderr, "Error: image name required\n"); return 1; }

    char mpath[PATH_MAX];
    manifest_path_for(image_name, mpath, sizeof(mpath));
    char *json_data = read_file(mpath);
    if (!json_data) {
        fprintf(stderr, "Error: image '%s' not found in local store\n", image_name);
        return 1;
    }
    cJSON *manifest = cJSON_Parse(json_data);
    free(json_data);
    if (!manifest) {
        fprintf(stderr, "Error: failed to parse manifest for '%s'\n", image_name);
        return 1;
    }
    cJSON *config = cJSON_GetObjectItem(manifest, "config");
    if (!config || !cJSON_IsObject(config)) {
        fprintf(stderr, "Error: manifest for '%s' is missing config\n", image_name);
        cJSON_Delete(manifest);
        return 1;
    }

    char temp_root[] = "/tmp/docksmith_run_XXXXXX";
    if (!mkdtemp(temp_root)) { perror("mkdtemp"); cJSON_Delete(manifest); return 1; }
    chmod(temp_root, 0755);

    int ret = 0;
    char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME is not set\n");
        ret = 1;
        goto run_done;
    }

    /* Extract every layer in manifest order (base layers are already included) */
    cJSON *layers = cJSON_GetObjectItem(manifest, "layers");
    cJSON *layer;
    cJSON_ArrayForEach(layer, layers) {
        cJSON *dig_item = cJSON_GetObjectItem(layer, "digest");
        if (!dig_item || !dig_item->valuestring) continue;
        const char *dig = dig_item->valuestring;
        if (strncmp(dig, "sha256:", 7) == 0) dig += 7;
        char lpath[PATH_MAX];
        snprintf(lpath, sizeof(lpath), "%s/.docksmith/layers/%s.tar", home, dig);
        if (extract_tar(lpath, temp_root) != 0) {
            fprintf(stderr, "Error: failed to extract layer %s\n", dig_item->valuestring);
            ret = 1; goto run_done;
        }
    }

    {
        /* Determine command argv */
        char **exec_argv = NULL;
        int exec_free = 0;

        if (cmd_start >= 0 && cmd_start < argc) {
            exec_argv = &argv[cmd_start];  /* direct exec, no sh -c */
        } else {
            cJSON *cmd_arr = cJSON_GetObjectItem(config, "Cmd");
            int cmd_size = cmd_arr ? cJSON_GetArraySize(cmd_arr) : 0;
            if (cmd_size == 0) {
                fprintf(stderr,
                    "Error: no command specified and image has no CMD (rubric §6)\n");
                ret = 1; goto run_done;
            }
            exec_argv = malloc((cmd_size + 1) * sizeof(char *));
            if (!exec_argv) { ret = 1; goto run_done; }
            exec_free = 1;
            for (int j = 0; j < cmd_size; j++) {
                cJSON *it = cJSON_GetArrayItem(cmd_arr, j);
                if (!it || !cJSON_IsString(it) || !it->valuestring) {
                    fprintf(stderr, "Error: manifest CMD must be an array of strings\n");
                    ret = 1;
                    free(exec_argv);
                    exec_free = 0;
                    goto run_done;
                }
                exec_argv[j] = it->valuestring;
            }
            exec_argv[cmd_size] = NULL;
        }

        /* Build env: manifest ENV then -e overrides (dedup by key) */
        char *final_env[128] = {NULL};
        int env_idx = 0;
        cJSON *manifest_envs = cJSON_GetObjectItem(config, "Env");
        cJSON *e_item;
        cJSON_ArrayForEach(e_item, manifest_envs) {
            if (!cJSON_IsString(e_item) || !e_item->valuestring) continue;
            if (env_idx >= 127) break;
            final_env[env_idx] = strdup(e_item->valuestring);
            if (!final_env[env_idx]) { ret = 1; goto run_cleanup_env; }
            env_idx++;
        }

        for (int i = 0; i < env_count; i++) {
            const char *eq = strchr(env_overrides[i], '=');
            size_t klen = eq ? (size_t)(eq - env_overrides[i]) : strlen(env_overrides[i]);
            int found = 0;
            for (int j = 0; j < env_idx; j++) {
                if (strncmp(final_env[j], env_overrides[i], klen) == 0 &&
                    final_env[j][klen] == '=') {
                    free(final_env[j]);
                    final_env[j] = strdup(env_overrides[i]);
                    if (!final_env[j]) { ret = 1; goto run_cleanup_env; }
                    found = 1; break;
                }
            }
            if (!found) {
                if (env_idx >= 127) break;
                final_env[env_idx] = strdup(env_overrides[i]);
                if (!final_env[env_idx]) { ret = 1; goto run_cleanup_env; }
                env_idx++;
            }
        }

        const char *workdir = "/";
        cJSON *wd_item = cJSON_GetObjectItem(config, "WorkingDir");
        if (wd_item && cJSON_IsString(wd_item) && wd_item->valuestring[0])
            workdir = wd_item->valuestring;

        ret = execute_isolated(temp_root, exec_argv, final_env, workdir);
        printf("Container exited with code: %d\n", ret);

run_cleanup_env:
        for (int i = 0; i < env_idx; i++) free(final_env[i]);
        if (exec_free) free(exec_argv);
        if (ret != 0) goto run_done;
    }

run_done:
    cleanup_run_root(temp_root);
    cJSON_Delete(manifest);
    return ret;
}
