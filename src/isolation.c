#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include "isolation.h"
#include "fsutil.h"

int execute_isolated(const char *new_root, char **argv, char **envp, const char *workdir) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        /* Abort on failure — no silent fallback that weakens isolation */
        if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS) != 0) {
            perror("unshare (needs root or user namespaces)");
            _exit(127);
        }
        if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
            perror("mount private");
            _exit(127);
        }
        if (chroot(new_root) != 0) { perror("chroot"); _exit(127); }
        if (chdir("/") != 0)       { perror("chdir /"); _exit(127); }

        /* mkdir_p syscalls only — no system() */
        if (workdir && workdir[0]) {
            mkdir_p(workdir, 0755);
            if (chdir(workdir) != 0) chdir("/");
        }

        mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

        extern char **environ;
        char **env = (envp && envp[0]) ? envp : environ;
        if (argv[0][0] == '/') execve(argv[0], argv, env);
        else                   execvpe(argv[0], argv, env);

        perror("exec");
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return -1; }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}
