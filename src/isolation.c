#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <unistd.h>
#include <string.h>
#include "isolation.h"

// Stack size for the cloned child process
#define STACK_SIZE (1024 * 1024)

struct container_config {
    char *rootfs_path;
    char **cmd;
    char **env;
    char *workdir;
};

// This function runs inside the isolated child process
static int child_exec(void *arg) {
    struct container_config *config = (struct container_config *)arg;

    // 1. Filesystem Isolation (Hard Requirement) [cite: 115]
    // We chroot into the assembled rootfs so the process cannot read/write outside [cite: 115]
    if (chroot(config->rootfs_path) != 0) {
        perror("chroot failed");
        return 1;
    }
    
    if (chdir(config->workdir != NULL ? config->workdir : "/") != 0) {
        perror("chdir failed");
        return 1;
    }

    // 2. Setup Environment Variables [cite: 119]
    // Clears host environment and sets image/runtime ENV variables
    clearenv();
    for (int i = 0; config->env != NULL && config->env[i] != NULL; i++) {
        putenv(config->env[i]);
    }

    // 3. Exec the command [cite: 114]
    if (execvp(config->cmd[0], config->cmd) == -1) {
        perror("execvp failed");
        return 1;
    }

    return 0;
}

// OS-level isolation primitive [cite: 127]
int execute_isolated(const char *rootfs, char *const cmd[], char *const env[], const char *workdir) {
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc failed for stack");
        return -1;
    }

    struct container_config config = {
        .rootfs_path = (char *)rootfs,
        .cmd = (char **)cmd,
        .env = (char **)env,
        .workdir = (char *)workdir
    };

    // Use clone to create a new process with its own mount namespace (CLONE_NEWNS)
    // and PID namespace (CLONE_NEWPID) for isolation.
    pid_t pid = clone(child_exec, stack + STACK_SIZE, CLONE_NEWNS | CLONE_NEWPID | SIGCHLD, &config);
    if (pid == -1) {
        perror("clone failed");
        free(stack);
        return -1;
    }

    // The CLI blocks until the process exits [cite: 121]
    int status;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid failed");
        free(stack);
        return -1;
    }

    free(stack);
    
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}