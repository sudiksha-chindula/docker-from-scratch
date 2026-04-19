#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <cjson/cJSON.h>
#include "build.h"
#include "isolation.h"
#include "tar_utils.h"

void compute_cache_key(char *out_hash, const char *prev, const char *ins, const char *wd, const char *env) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, prev, strlen(prev));
    EVP_DigestUpdate(ctx, ins, strlen(ins));
    EVP_DigestUpdate(ctx, wd, strlen(wd));
    unsigned char md[32]; unsigned int len;
    EVP_DigestFinal_ex(ctx, md, &len);
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < 32; i++) sprintf(&out_hash[i*2], "%02x", md[i]);
    out_hash[64] = '\0';
}

int execute_build(const char *tag, const char *context, int use_cache) {
    char abs_ctx[4096]; realpath(context, abs_ctx);
    char dfile[4110]; snprintf(dfile, 4110, "%s/Docksmithfile", abs_ctx);
    FILE *f = fopen(dfile, "r"); if (!f) return 1;

    cJSON *man = cJSON_CreateObject();
    cJSON_AddStringToObject(man, "name", tag);
    cJSON_AddItemToObject(man, "config", cJSON_CreateObject());
    cJSON_AddStringToObject(cJSON_GetObjectItem(man, "config"), "WorkingDir", "/");
    cJSON_AddItemToObject(cJSON_GetObjectItem(man, "config"), "Cmd", cJSON_CreateArray());
    cJSON_AddItemToObject(man, "layers", cJSON_CreateArray());

    char line[1024], wd[1024] = "/", prev[65] = "000";
    int step = 1;

    while (fgets(line, 1024, f)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '#' || strlen(line) == 0) continue;
        printf("Step %d: %s\n", step++, line);

        if (strncmp(line, "FROM", 4) == 0) {
            // FORCE CLEAN: Lazy unmount any leaks before deleting
            system("sudo umount -l /tmp/docksmith_build_root/proc 2>/dev/null");
            system("sudo rm -rf /tmp/docksmith_build_root && mkdir -p /tmp/docksmith_build_root");
            char base[4096]; snprintf(base, 4096, "%s/.docksmith/layers/alpine.tar", getenv("HOME"));
            extract_tar(base, "/tmp/docksmith_build_root");
            system("cp /etc/resolv.conf /tmp/docksmith_build_root/etc/resolv.conf");
        } else if (strncmp(line, "WORKDIR", 7) == 0) {
            sscanf(line, "WORKDIR %s", wd);
            cJSON_ReplaceItemInObject(cJSON_GetObjectItem(man, "config"), "WorkingDir", cJSON_CreateString(wd));
        } else if (strncmp(line, "CMD", 3) == 0) {
            cJSON_AddItemToArray(cJSON_GetObjectItem(cJSON_GetObjectItem(man, "config"), "Cmd"), cJSON_CreateString(line + 4));
        } else {
            char key[65]; compute_cache_key(key, prev, line, wd, "");
            if (strncmp(line, "RUN", 3) == 0) {
                char *args[] = {"/bin/sh", "-c", line + 4, NULL};
                if (execute_isolated("/tmp/docksmith_build_root", args, NULL, wd) != 0) return 1;
            } else {
                char s[256], d[256], cp[8192]; sscanf(line, "COPY %s %s", s, d);
                snprintf(cp, 8192, "cp -r %s/%s /tmp/docksmith_build_root%s", abs_ctx, s, d);
                system(cp);
            }
            char tp[4096]; snprintf(tp, 4096, "%s/.docksmith/layers/%s.tar", getenv("HOME"), key);
            if (create_layer_tar(tp, "/tmp/docksmith_build_root") != 0) {
                fprintf(stderr, "Fatal: Layer capture failed.\n");
                return 1;
            }
            strcpy(prev, key);
            cJSON *l = cJSON_CreateObject();
            char dstr[80]; snprintf(dstr, 80, "sha256:%s", key);
            cJSON_AddStringToObject(l, "digest", dstr);
            cJSON_AddItemToArray(cJSON_GetObjectItem(man, "layers"), l);
        }
    }

    char mpath[4096]; snprintf(mpath, 4096, "%s/.docksmith/images/%s.json", getenv("HOME"), tag);
    system("mkdir -p ~/.docksmith/images");
    FILE *mf = fopen(mpath, "w"); fprintf(mf, "%s", cJSON_Print(man)); fclose(mf);
    printf("Successfully built image: %s\n", tag);
    return 0;
}
