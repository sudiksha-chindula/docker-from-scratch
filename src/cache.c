#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "cache.h"

static int path_for_cache_key(const char *key, char out[4096]) {
    const char *home = getenv("HOME");
    if (!home || !key || !key[0]) return -1;
    int n = snprintf(out, 4096, "%s/.docksmith/cache/%s", home, key);
    if (n < 0 || n >= 4096) return -1;
    return 0;
}

static int path_for_layer(const char *digest, char out[4096]) {
    const char *home = getenv("HOME");
    if (!home || !digest || !digest[0]) return -1;
    int n = snprintf(out, 4096, "%s/.docksmith/layers/%s.tar", home, digest);
    if (n < 0 || n >= 4096) return -1;
    return 0;
}

char *cache_lookup(const char *key) {
    char cpath[4096];
    if (path_for_cache_key(key, cpath) != 0) return NULL;

    FILE *f = fopen(cpath, "r");
    if (!f) return NULL;

    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0') return NULL;

    char lpath[4096];
    if (path_for_layer(line, lpath) != 0) return NULL;
    struct stat st;
    if (stat(lpath, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;

    return strdup(line);
}

int cache_store(const char *key, const char *layer_digest) {
    char cpath[4096];
    if (path_for_cache_key(key, cpath) != 0 || !layer_digest || !layer_digest[0]) return -1;

    int fd = open(cpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    size_t n = strlen(layer_digest);
    if (write(fd, layer_digest, n) != (ssize_t)n || write(fd, "\n", 1) != 1) {
        close(fd);
        return -1;
    }

    if (close(fd) != 0) return -1;
    return 0;
}
