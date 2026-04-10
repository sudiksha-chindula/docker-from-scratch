#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

char* keyword;
char buffer[100];
char* line;
char* remline=NULL;
char* remtok;
size_t toklen;
size_t linelen;
struct parsed* all[50];


int main()
{
    int count=5;
    char path[300]="/Users/sudiksha/sudiksha/acad/sem-6/cloud-computing/project/docker-from-scratch/src/Docksmithfile";
    struct parsed* result = parse(path, &count);
    printf("Result returned successfully.\n");
    printf("Instr: %s\n", result[0].instr_line);
    printf("Instr keyword: %s\n", result[0].instr_keyword);
    printf("Instr remline: %s\n", result[0].instr_remline);
    printf("Type: %d", result[0].instrtype);
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
        //line extraction
        main[i].instr_line=line;
        printf("Line number: %d\n", i);
        printf("Line: %s", line);
        linelen+=strlen(line);

        //keyword determination
        keyword = strtok(line, " ");
        printf("keyword: %s\n", keyword);
        main[i].instr_keyword=keyword;
        
        //remaining line (token) determination
        remtok=keyword;
        while((remtok=strtok(NULL, " "))!=NULL)    //loop check after strtok
        {
            printf("remtok: %s\n", remtok);
            toklen+=strlen(remtok);
            remline=realloc(remline, toklen);
            remline = strcat(remline, remtok);
        }
        main[i].instr_remline=remline;

        //instruction type determination
        if (strcmp(keyword, "FROM")==0)
        {
            main[i].instrtype=0;
        }
        else if (strcmp(keyword,"COPY")==0)
        {
            main[i].instrtype=1;
        }
        else if (strcmp(keyword, "RUN")==0)
        {
            main[i].instrtype=2;
        }
        else if (strcmp(keyword,"WORKDIR")==0)
        {
            main[i].instrtype=3;
        }
        else if (strcmp(keyword, "ENV")==0)
        {
            main[i].instrtype=4;
        }
        else 
        {
            printf("Invalid instruction in Docksmithfile. Please check.\n");
        }
        i++;
    
        
        line=fgets(buffer, sizeof(buffer), ds_file);
        printf("\n");
    }
    return main;
}