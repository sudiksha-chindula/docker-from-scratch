#ifndef BUILD_H
#define BUILD_H

#include <cjson/cJSON.h>

typedef enum {
    INSTR_FROM, INSTR_COPY, INSTR_RUN, INSTR_WORKDIR, INSTR_ENV, INSTR_CMD
} instr_kind_t;

typedef struct {
    instr_kind_t  kind;
    int           line_no;
    char         *raw;           /* full line, for cache key + display */
    char         *from_image;    /* INSTR_FROM */
    char         *from_tag;      /* INSTR_FROM: default "latest" */
    char         *copy_src;      /* INSTR_COPY */
    char         *copy_dest;     /* INSTR_COPY */
    char         *run_cmd;       /* INSTR_RUN: everything after "RUN " */
    char         *workdir_path;  /* INSTR_WORKDIR */
    char         *env_key;       /* INSTR_ENV */
    char         *env_value;     /* INSTR_ENV */
    char        **cmd_argv;      /* INSTR_CMD: NULL-terminated parsed array */
} instruction_t;

/* Parse a Docksmithfile. Caller frees via free_instructions(). Returns 0 on success. */
int  parse_docksmithfile(const char *path, instruction_t **out_list, int *out_count);
void free_instructions(instruction_t *list, int count);

int execute_build(const char *tag, const char *context_dir, int use_cache);

#endif /* BUILD_H */
