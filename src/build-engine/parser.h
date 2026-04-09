#ifndef PARSER_H
#define PARSER_H

enum type
{
    FROM,
    COPY,
    RUN,
    WORKDIR,
    ENV,
    CMD
};

struct parsed
{
    char* instr;
    enum type instrtype;
};

struct parsed* parse(char*, int*);

#endif