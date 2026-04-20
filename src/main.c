#include <stdio.h>
#include <string.h>
#include "cli.h"
#include "state.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: docksmith <command> [args...]\n");
        return 1;
    }

    // Ensure ~/.docksmith/ exists on disk
    init_state_directory();

    if (strcmp(argv[1], "build") == 0) {
        return handle_build(argc, argv);
    } else if (strcmp(argv[1], "run") == 0) {
        return handle_run(argc, argv);
    } else if (strcmp(argv[1], "images") == 0) {
        return handle_images(argc, argv);
    } else if (strcmp(argv[1], "rmi") == 0) {
        return handle_rmi(argc, argv);
    } else if (strcmp(argv[1], "import") == 0) {
        return handle_import(argc, argv);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
}