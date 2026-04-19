#include <stdio.h>
#include <string.h>
#include "cli.h"
#include "build.h"
#include "isolation.h"

int handle_build(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: docksmith build -t <name:tag> <context> [--no-cache]\n");
        return 1;
    }

    char *tag = NULL;
    char *context = NULL;
    int use_cache = 1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            tag = argv[++i];
        } else if (strcmp(argv[i], "--no-cache") == 0) {
            use_cache = 0; // Skip all cache lookups [cite: 124]
        } else {
            context = argv[i];
        }
    }

    if (!tag || !context) {
        fprintf(stderr, "Error: Missing tag or context.\n");
        return 1;
    }

    printf("Building %s from %s...\n", tag, context);
    return execute_build(tag, context, use_cache);
}

int handle_run(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: docksmith run [-e KEY=VALUE] <name:tag> [cmd]\n");
        return 1;
    }
    // Stub: Parse -e flags, locate image manifest, extract layers, and call execute_isolated()[cite: 114, 119].
    printf("Running container...\n");
    return 0;
}

int handle_images(int argc, char **argv) {
    // Stub: Read ~/.docksmith/images/ and list contents[cite: 125].
    printf("Listing images...\n");
    printf("%-20s %-10s %-15s %s\n", "NAME", "TAG", "ID", "CREATED");
    return 0;
}

int handle_rmi(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: docksmith rmi <name:tag>\n");
        return 1;
    }
    // Stub: Delete manifest and associated layers[cite: 125, 127].
    printf("Removing image %s...\n", argv[2]);
    return 0;
}