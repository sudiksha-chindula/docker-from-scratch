#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cjson/cJSON.h>
#include "cli.h"
#include "build.h"
#include "isolation.h"
#include "tar_utils.h"

// Helper to read manifest into memory
char* read_file(const char* filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(length + 1);
    if (data) {
        fread(data, 1, length, f);
        data[length] = '\0';
    }
    fclose(f);
    return data;
}

int handle_build(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: docksmith build -t <tag> <context> [--no-cache]\n");
        return 1;
    }
    char *tag = NULL, *context = NULL;
    int use_cache = 1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) tag = argv[++i];
        else if (strcmp(argv[i], "--no-cache") == 0) use_cache = 0;
        else context = argv[i];
    }
    return execute_build(tag, context, use_cache);
}

int handle_images(int argc, char **argv) {
    (void)argc; (void)argv;
    char path[512];
    snprintf(path, sizeof(path), "%s/.docksmith/images/", getenv("HOME"));
    DIR *dir = opendir(path);
    if (!dir) {
        printf("No images found. Build one first!\n");
        return 0;
    }

    printf("%-20s %-10s %-15s\n", "NAME", "TAG", "IMAGE ID");
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strstr(ent->d_name, ".json")) {
            char filepath[1024];
            snprintf(filepath, sizeof(filepath), "%s%s", path, ent->d_name);
            char *json_data = read_file(filepath);
            
            if (json_data) {
                cJSON *manifest = cJSON_Parse(json_data);
                if (manifest) {
                    // Check if items exist before accessing valuestring
                    cJSON *name = cJSON_GetObjectItem(manifest, "name");
                    cJSON *tag = cJSON_GetObjectItem(manifest, "tag");
                    cJSON *digest = cJSON_GetObjectItem(manifest, "digest");

                    printf("%-20s %-10s %-15.12s\n", 
                           name ? name->valuestring : "none",
                           tag ? tag->valuestring : "latest",
                           digest ? digest->valuestring : "unknown");
                    
                    cJSON_Delete(manifest);
                }
                free(json_data);
            }
        }
    }
    closedir(dir);
    return 0;
}

int handle_rmi(int argc, char **argv) {
    if (argc < 3) return 1;
    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s/.docksmith/images/%s.json", getenv("HOME"), argv[2]);
    if (remove(manifest_path) == 0) {
        printf("Untagged and deleted: %s\n", argv[2]);
        return 0;
    }
    fprintf(stderr, "Error: Could not delete image %s\n", argv[2]);
    return 1;
}

int handle_run(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: docksmith run [-e KEY=VAL] <image> [cmd]\n");
        return 1;
    }

    char *image_name = NULL;
    char *runtime_cmd_override = NULL;
    char *env_overrides[64] = {NULL};
    int env_count = 0;

    // 1. Improved argument parsing
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            env_overrides[env_count++] = argv[++i];
        } else if (!image_name) {
            image_name = argv[i];
        } else {
            runtime_cmd_override = argv[i];
        }
    }

    char manifest_path[512], base_tar[512];
    char *home = getenv("HOME");
    snprintf(manifest_path, sizeof(manifest_path), "%s/.docksmith/images/%s.json", home, image_name);
    snprintf(base_tar, sizeof(base_tar), "%s/.docksmith/layers/alpine.tar", home);

    char *json_data = read_file(manifest_path);
    if (!json_data) {
        fprintf(stderr, "Error: Image %s not found\n", image_name);
        return 1;
    }
    cJSON *manifest = cJSON_Parse(json_data);
    cJSON *config = cJSON_GetObjectItem(manifest, "config");

    // 2. Create and secure temporary runtime root
    char temp_root[] = "/tmp/docksmith_run_XXXXXX";
    if (mkdtemp(temp_root) == NULL) {
        perror("Failed to create temporary runtime root");
        return 1;
    }
    chmod(temp_root, 0755);

    // 3. Assemble Filesystem (Base + Layers)
    printf("Assembling rootfs for %s...\n", image_name);
    if (extract_tar(base_tar, temp_root) != 0) {
        fprintf(stderr, "Error: Base image extraction failed\n");
        return 1;
    }

    cJSON *layers = cJSON_GetObjectItem(manifest, "layers");
    cJSON *layer;
    cJSON_ArrayForEach(layer, layers) {
        char layer_path[1024];
        const char* digest = cJSON_GetObjectItem(layer, "digest")->valuestring;
        // Skip 'sha256:' prefix
        snprintf(layer_path, sizeof(layer_path), "%s/.docksmith/layers/%s.tar", home, digest + 7);
        if (extract_tar(layer_path, temp_root) != 0) {
            fprintf(stderr, "Error: Failed to extract layer %s\n", digest);
            return 1;
        }
    }

    // 4. Setup Execution Context (WorkingDir, CMD, Env)
    const char *workdir = cJSON_GetObjectItem(config, "WorkingDir") ? cJSON_GetObjectItem(config, "WorkingDir")->valuestring : "/";
    
    char *exec_cmd[4] = {"/bin/sh", "-c", NULL, NULL};
    if (runtime_cmd_override) {
        exec_cmd[2] = runtime_cmd_override;
    } else {
        cJSON *cmd_node = cJSON_GetArrayItem(cJSON_GetObjectItem(config, "Cmd"), 0);
        if (!cmd_node) {
            fprintf(stderr, "Error: Image has no CMD defined\n");
            return 1;
        }
        exec_cmd[2] = cmd_node->valuestring;
    }

    // Assemble Environment
    char *final_env[128] = {NULL};
    int env_idx = 0;
    cJSON *manifest_envs = cJSON_GetObjectItem(config, "Env");
    cJSON *e_item;
    cJSON_ArrayForEach(e_item, manifest_envs) {
        final_env[env_idx++] = strdup(e_item->valuestring);
    }
    for (int i = 0; i < env_count; i++) {
        final_env[env_idx++] = strdup(env_overrides[i]);
    }

    // 5. BOOT
    printf("Starting container %s...\n", image_name);
    int status = execute_isolated(temp_root, exec_cmd, final_env, workdir);
    printf("Container exited with code: %d\n", status);

    // 6. Cleanup
    char rm_cmd[1024];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", temp_root);
    system(rm_cmd);

    // Memory Cleanup
    for (int i = 0; i < env_idx; i++) free(final_env[i]);
    cJSON_Delete(manifest);
    free(json_data);

    return status;
}
