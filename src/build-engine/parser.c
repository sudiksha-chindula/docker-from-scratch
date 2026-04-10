#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

char* keyword;
char buffer[100];
char* line;
char* remline;
char* remtok;
struct parsed* all[50];


int main()
{
    int count=5;
    char path[300]="/Users/sudiksha/sudiksha/acad/sem-6/cloud-computing/project/docker-from-scratch/src/Docksmithfile";
    struct parsed* result = parse(path, &count);
    printf("Result returned successfully.\n");
    printf("Instr: %s", result->instr);
    printf("Type: %d", result->instrtype);
    return 0;
}

struct parsed* parse(char* path, int* count)
{
    FILE* ds_file=fopen(path, "r");
    int i=0;

    //file pointer null case
    if (ds_file==NULL)
    {
        perror("fopen");
        return NULL;
    }

    struct parsed* main = malloc(sizeof(struct parsed)*50);
    line = fgets(buffer, sizeof(buffer), ds_file);

    while (line!=NULL)
    {
        main[i].instr=line;
        printf("Line number: %d\n", i);

        //keyword determination
        keyword = strtok(line, " ");
        printf("keyword: %s\n", keyword);
        
        //remaining line determination
        remtok=keyword;
        while((remtok=strtok(NULL, " "))!=NULL)    //loop check after strtok
        {
            printf("remtok: %s\n", remtok);
        }
        
        //instruction type determination
        if (strcmp(keyword, "FROM")==0)
        {
            main[i].instrtype=FROM;
        }
        else if (strcmp(keyword,"COPY")==0)
        {
            main[i].instrtype=COPY;
        }
        else if (strcmp(keyword, "RUN")==0)
        {
            main[i].instrtype=RUN;
        }
        else if (strcmp(keyword,"WORKDIR")==0)
        {
            main[i].instrtype=WORKDIR;
        }
        else if (strcmp(keyword, "ENV")==0)
        {
            main[i].instrtype=ENV;
        }
        else 
        {
            printf("Invalid instruction in Docksmithfile. Please check.\n");
        }
        i++;
    
        
        line=fgets(buffer, sizeof(buffer), ds_file);
        
    }
    return main;
}