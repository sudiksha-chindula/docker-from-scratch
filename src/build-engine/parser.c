#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

char keyword[256];
char remline[256];
char buffer[100];

int main()
{
    int count=5;
    char path[300]="/Users/sudiksha/sudiksha/acad/sem-6/cloud-computing/project/docker-from-scratch/src/Docksmithfile";
    struct parsed* result = parse(path, &count);
    printf("Result returned successfully");
    return 0;
}

struct parsed* parse(char* path, int* count)
{
    FILE* ds_file=fopen(path, "r");
    int i=0;

    //file pointer null
    if (ds_file==NULL)
    {
        perror("fopen");
        return NULL;
    }


    while (fgets(buffer, sizeof(buffer), ds_file)!=NULL)
    {
        char* line = fgets(buffer, sizeof(buffer), ds_file);
        sscanf(buffer, "%s %[^\n]", keyword, remline);
        
        printf("%s keyword\n", keyword);
        printf("%s remaining line\n", line);

    }
    //temp return
    struct parsed* i1 = malloc(sizeof(struct parsed));
    i1->instr=keyword;
    strcpy(i1->instrtype, "COPY");
    return i1;
}