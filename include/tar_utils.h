#ifndef TAR_UTILS_H
#define TAR_UTILS_H

typedef struct {
    char **paths; /* relative paths inside src_dir */
    int count;
} path_set_t;

// Extracts a tarball to a specific destination directory
int extract_tar(const char *tar_filename, const char *dest_dir);

// Creates a deterministic tarball from a source directory 
// (Timestamps zeroed out for reproducible cache hashes)
int create_layer_tar(const char *tar_filename, const char *src_dir);

/* Creates deterministic tarball from only selected relative paths. */
int create_delta_tar(const char *tar_filename, const char *src_dir,
                     const path_set_t *paths_to_include);

#endif // TAR_UTILS_H
