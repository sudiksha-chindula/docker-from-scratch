#ifndef PARSER_H
#define PARSER_H

struct parsed
{
    char* instr_line;
    char* instr_keyword;
    char* instr_remline;
    int instrtype;
};

struct parsed* parse(char*, int*);

#endif