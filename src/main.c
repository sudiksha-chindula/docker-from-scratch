#include <stdio.h>
#include <string.h>

void ds_build(char* []);
void ds_images(char* []);
void ds_run(char* []);
void ds_rmi(char* []);

int main(int argc, char* argv[])
{
    if (strcmp(argv[1], "docksmith")==0)    //docksmith
    {       
        if (strcmp(argv[2], "build")==0)        //docksmith build
        {
            ds_build(argv);
        }
        else if (strcmp(argv[2], "images")==0)  //docksmith images
        {
            ds_images(argv);
        }
        else if (strcmp(argv[2], "run")==0)     //docksmith run
        {
            ds_run(argv);
        }
        else if (strcmp(argv[2], "rmi")==0)     //docksmith rmi
        {
            ds_rmi(argv);
        }
    }
    //for (int i=1; i<argc; i++)
    //{}
    return 0;
}

void ds_build(char* argv[])
{
    printf("%s", "build: not implemented\n");
}

void ds_images(char* argv[])
{
    printf("%s", "build: not implemented\n");
}

void ds_rmi(char* argv[])
{
    printf("%s", "build: not implemented\n");
}

void ds_run(char* argv[])
{
    printf("%s", "build: not implemented\n");
}