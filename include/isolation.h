#ifndef ISOLATION_H
#define ISOLATION_H

// Executes a command inside an isolated filesystem root[cite: 114, 115].
int execute_isolated(const char *rootfs, char *const cmd[], char *const env[], const char *workdir);

#endif // ISOLATION_H