#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include "state.h"

void init_state_directory() {
    char *home = getenv("HOME");
    char path[512];

    // Create state directory layout [cite: 9]
    const char* dirs[] = {
        "/.docksmith",
        "/.docksmith/images",     // [cite: 11]
        "/.docksmith/layers",     // [cite: 12]
        "/.docksmith/cache"       // [cite: 13]
    };

    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s%s", home, dirs[i]);
        mkdir(path, 0755); // Ignore errors if they already exist
    }
}