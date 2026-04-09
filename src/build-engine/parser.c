#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

char* keyword;
char buffer[100];
char* keyword;
char* line;
char* remline;
struct parsed* all[50];

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
    struct parsed* current = malloc(sizeof(struct parsed));
    do
    {
        line = fgets(buffer, sizeof(buffer), ds_file);
        printf("%s", line);
        keyword = strtok(line, " ");
        remline=keyword;
        printf("keyword: %s\n", keyword);
       
        while(remline!=NULL)
        {
            remline = strtok(NULL, " ");
            printf("remline: %s\n", remline);
        }
        
        current->instr=line;
        if (strcmp(keyword, "FROM")==0)
        {
            current->instrtype=FROM;
        }
        else if (strcmp(keyword,"COPY"))
        {
            current->instrtype=COPY;
        }
        else if (strcmp(keyword, "RUN")==0)
        {
            current->instrtype=RUN;
        }
        else if (strcmp(keyword,"WORKDIR")==0)
        {
            current->instrtype=WORKDIR;
        }
        else if (strcmp(keyword, "ENV")==0)
        {
            current->instrtype=ENV;
        }
        else
        {
            printf("Invalid instruction in Docksmithfile. Please check.\n");
        }
        all[i]=current;
        i++;
        //sscanf(buffer, "%s %[^\n]", keyword, remline);
        //printf("%s keyword\n", keyword);
        //printf("%s remaining line\n", line);
        
        
    }
    while (line!=NULL);

    //temp return
    return current;
}