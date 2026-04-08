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

struct parsed_instr
{
    char* instrarg;
    enum type instrtype;
};

struct parsed_instr* parse_instr(char*, int*);

#endif