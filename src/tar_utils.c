#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <archive.h>
#include <archive_entry.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <libgen.h>
#include "tar_utils.h"

int extract_tar(const char *tar_filename, const char *dest_dir) {
    struct archive *a = archive_read_new();
    struct archive *ext = archive_write_disk_new();
    struct archive_entry *entry;
    archive_read_support_format_tar(a);
    archive_read_support_filter_all(a);
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);

    if (archive_read_open_filename(a, tar_filename, 10240) != ARCHIVE_OK) return 1;
    char cwd[4096]; getcwd(cwd, sizeof(cwd));
    chdir(dest_dir);

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        archive_write_header(ext, entry);
        if (archive_entry_size(entry) > 0) {
            const void *buff; size_t size; int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK)
                archive_write_data_block(ext, buff, size, offset);
        }
        archive_write_finish_entry(ext);
    }
    archive_read_close(a); archive_read_free(a);
    archive_write_close(ext); archive_write_free(ext);
    chdir(cwd);
    return 0;
}

int create_layer_tar(const char *tar_filename, const char *src_dir) {
    // 1. Ensure directory exists
    char dir_buf[4096]; strncpy(dir_buf, tar_filename, 4096);
    char mkdir_cmd[4100];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dirname(dir_buf));
    system(mkdir_cmd);

    // 2. The tar command
    // --one-file-system: Prevents tar from trying to archive /proc even if it's mounted
    // --warning=no-file-changed: Ignores minor apk metadata shifts
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), 
             "tar --one-file-system --warning=no-file-changed -cf %s -C %s .", 
             tar_filename, src_dir);
    
    int status = system(cmd);
    return (WEXITSTATUS(status) <= 1) ? 0 : 1;
}
