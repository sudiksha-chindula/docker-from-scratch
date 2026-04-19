#ifndef TAR_UTILS_H
#define TAR_UTILS_H

// Extracts a tarball to a specific destination directory
int extract_tar(const char *tar_filename, const char *dest_dir);

// Creates a deterministic tarball from a source directory 
// (Timestamps zeroed out for reproducible cache hashes)
int create_layer_tar(const char *tar_filename, const char *src_dir);

#endif // TAR_UTILS_H
