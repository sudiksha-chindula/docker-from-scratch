#ifndef FSUTIL_H
#define FSUTIL_H
#include <sys/types.h>

/* Recursive mkdir — creates every component, ignores EEXIST. Returns 0 on success. */
int mkdir_p(const char *path, mode_t mode);

/*
 * Copy files matched by src_pattern (relative to context_dir) into
 * dest_in_root inside temp_root.  Supports glob(3) and ** recursive globs.
 * Returns 0 on success, -1 on error (including path-escape attempts).
 */
int copy_into_rootfs(const char *context_dir, const char *src_pattern,
                     const char *dest_in_root, const char *temp_root);

#endif /* FSUTIL_H */
