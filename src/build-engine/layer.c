#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <archive.h>
#include <archive_entry.h>
#include <string.h>
#include <stdlib.h>

char *filename; //appended to dir path
char afilename[256];
char dir_name[2048];

const char *outname = "dummy-dir.tar.gz";

//refer libarchive documentation (rem to add to README)
void write_archive(const char *outname, const char *path, const char* name)
{
    struct archive *a;
    struct archive_entry *entry;
    struct stat st;
    char buff[8192];
    size_t len;
    FILE *fd;

    //file stat data check
    if (stat(path, &st) != 0)
    {
        perror("stat");
        return;
    }

    //creating archive objects
    a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);

    if (archive_write_open_filename(a, outname) != ARCHIVE_OK)
    {
        printf("Could not open archive output file\n");
        archive_write_free(a);
        return;
    }

    //creating archive entry
    entry = archive_entry_new();

    archive_entry_set_pathname(entry, name);
    archive_entry_set_size(entry, st.st_size);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);

    archive_write_header(a, entry);

    //file archive write
    fd = fopen(path, "rb");
    if (!fd)
    {
        perror("fopen");
        archive_entry_free(entry);
        archive_write_close(a);
        archive_write_free(a);
        return;
    }

    //archive data write
    while ((len = fread(buff, 1, sizeof(buff), fd)) > 0)
    {
        archive_write_data(a, buff, len);
    }

    fclose(fd);
    archive_entry_free(entry);

    printf("Successfully wrote entry: %s\n", name);

    archive_write_close(a);
    archive_write_free(a);
}

int main()
{
    strcpy(dir_name,
    "/Users/sudiksha/sudiksha/acad/sem-6/cloud-computing/project/docker-from-scratch/src/dummy-dir");

    struct dirent *dp;
    DIR *dfd = opendir(dir_name);

    if (dfd == NULL)
    {
        printf("Invalid Directory, couldn't be resolved\n");
        return 1;
    }

    strcat(dir_name, "/");

    printf("Reading directory: %s\n", dir_name);

    while ((dp = readdir(dfd)) != NULL)
    {
        filename = dp->d_name;

        if (strcmp(filename, ".") != 0 &&
            strcmp(filename, "..") != 0)
        {
            strcpy(afilename, dir_name);
            strcat(afilename, filename);

            write_archive(outname, afilename, filename);
        }
    }

    closedir(dfd);
    return 0;
}