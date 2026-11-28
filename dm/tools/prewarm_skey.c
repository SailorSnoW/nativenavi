// The host's prewarm key parser for scripts/merge_prewarm_seeds.py: one "R\t<hex>" or "M\t<hex>" per line in, the shaders and stages it names and its key as the
// host normalizes it out (pso_skey_parse / pso_mesh_skey_parse, then pso_skey_hex), or "-" for a row the host would skip.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>

typedef struct { int stage; char key[24]; } NvShader;   // what nvn_pso_skey.h's writer reads of the host's NvShader
#include "nvn_pso_skey.h"

static int library_name(const char key[24]) {
    return strspn(key, "0123456789abcdef") == 16 && !strcmp(key + 16, "_b");
}

int main(void) {
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, stdin) >= 0) {
        line[strcspn(line, "\n")] = '\0';
        pso_skey key;
        if (line[0] == '\0' || line[1] != '\t' || (line[0] != 'R' && line[0] != 'M') ||
            (line[0] == 'R' ? pso_skey_parse(line + 2, &key) : pso_mesh_skey_parse(line + 2, &key)) ||
            !memchr(key.vs, 0, sizeof key.vs) || !memchr(key.fs, 0, sizeof key.fs) ||
            !library_name(key.vs) || (key.fs[0] && !library_name(key.fs)) ||
            key.k.vs || key.k.fs || key.k.fsfn) {
            puts("-");
            continue;
        }
        char hex[sizeof key * 2 + 1];
        pso_skey_hex(&key, hex);
        printf("%s\t%u\t%s\t%u\t%s\n", key.vs, key.vs_stage, key.fs, key.fs_stage, hex);
    }
    free(line);
    return ferror(stdin) || fflush(stdout) == EOF;
}
