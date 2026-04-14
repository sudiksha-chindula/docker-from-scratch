#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>   //temporarily unused
#include <archive.h>
#include <archive_entry.h>
#include <string.h>
#include <stdlib.h>

//executor engine returns the directory path

char* filename;
char afilename[256];
char dir_name[2048];
//char* filenames[100];
struct archive* a;
struct archive_entry* entry;
const char* outname = "/Users/sudiksha/sudiksha/acad/sem-6/cloud-computing/project/docker-from-scratch/src/dummy-dir.tar";
struct stat st;
char buff[8192];
int len;
FILE* fd;


void write_archive(const char *outname, const char **afilename)
{
  struct archive *a;
  struct archive_entry *entry;
  struct stat st;
  char buff[8192];
  int len;
  int fd;

  a = archive_write_new();
  archive_write_add_filter_gzip(a);
  archive_write_set_format_pax_restricted(a); // Note 1
  archive_write_open_filename(a, outname);
  while (*filename) {
    stat(*filename, &st);
    entry = archive_entry_new(); // Note 2
    archive_entry_set_pathname(entry, *filename);
    archive_entry_set_size(entry, st.st_size); // Note 3
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_write_header(a, entry);
    FILE* fd = fopen(*filename, "r");
    len = read(fd, buff, sizeof(buff));
    while ( len > 0 ) {
        archive_write_data(a, buff, len);
        len = read(fd, buff, sizeof(buff));
    }
    close(fd);
    archive_entry_free(entry);
    filename++;
  }
  archive_write_close(a); // Note 4
  archive_write_free(a); // Note 5
}

int main()
{
    strcpy(dir_name,"/Users/sudiksha/sudiksha/acad/sem-6/cloud-computing/project/docker-from-scratch/src/dummy-dir");
    struct dirent* dp;
    DIR *dfd;

    dfd = opendir(dir_name);

    //null condition check for directory pointer
    if (dfd==NULL)
    {
        printf("Invalid Directory, couldn't be resolved\n");
        return 0;
    }
    else
    {
        strcat(dir_name, "/");
        printf("%s\n", dir_name);
        while ((dp = readdir(dfd))!=NULL)
        {
            filename = dp->d_name;
            if (strcmp(filename, ".")!=0 && strcmp(filename, "..")!=0)
            {

                //giving complete file path in afilename
                strcpy(afilename, dir_name);
                strcat(afilename, filename);

            }                
            }
        }
        return 0;
        
    }

   

