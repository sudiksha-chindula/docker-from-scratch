#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <string.h>
#include <errno.h>
#include "isolation.h"

int execute_isolated(const char *new_root, char **argv, char **envp, const char *workdir) {
    pid_t pid = fork();
    if (pid < 0) return 1;

    if (pid == 0) {
        // 1. Child enters private namespaces
        // We use CLONE_NEWNS to make mounts private to this child ONLY
        if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS) != 0) {
            unshare(CLONE_NEWNS | CLONE_NEWUTS); // Fallback
        }

        // 2. CRITICAL: Prevent mount leakage to the host Ubuntu
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

        // 3. Enter the jail
        if (chroot(new_root) != 0) { perror("chroot"); exit(1); }
        
        // Ensure workdir exists and move into it
        char mkdir_cmd[1024];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", workdir);
        system(mkdir_cmd);
        if (chdir(workdir) != 0) chdir("/");

        // 4. Mount /proc (This is now invisible to the host!)
        mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

        // 5. Execute
        if (envp && envp[0]) execvpe(argv[0], argv, envp);
        else execvp(argv[0], argv);
        
        exit(1); 
    }

    int status;
    waitpid(pid, &status, 0);
    return WEXITSTATUS(status);
}
