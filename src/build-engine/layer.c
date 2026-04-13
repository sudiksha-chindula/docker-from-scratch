#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>   //temporarily unused
#include <archive.h>
#include <archive_entry.h>
#include <string.h>
#include <stdlib.h>

//executor engine returns the directory path

char* filename;
//char* filenames[100];

int main()
{
    char* dir_name = "/Users/sudiksha/sudiksha/acad/sem-6/cloud-computing/project/docker-from-scratch/src/dummy-dir";
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
        while ((dp = readdir(dfd))!=NULL)
        {
            filename = dp->d_name;
            //filenames[i++]=strdup(dp->d_name);
            //printf("%s\n", dp->d_name);
            if (strcmp(filename, ".")!=0 && strcmp(filename, "..")!=0)
            {
                printf("Successful.\n");
            }
        }
        
    }

    return 0;
}


