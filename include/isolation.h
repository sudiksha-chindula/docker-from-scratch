#ifndef ISOLATION_H
#define ISOLATION_H

// Executes a command in a chroot jail with its own PID and Mount namespaces
int execute_isolated(const char *rootfs, char **cmd, char **env, const char *workdir);

#endif
