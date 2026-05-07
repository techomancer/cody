#include "../src/tools.h"
#include "../src/json.h"
#include <stdio.h>
#include <stdlib.h>      /* system, remove */
#include <string.h>
#include <dos.h>         /* _dos_findfirst, _dos_findnext, _find_t */
#include <direct.h>      /* getcwd */

/* -----------------------------------------------------------------------
   read_file
   ----------------------------------------------------------------------- */

static int tool_read_file(const char *argsJson, char *result,
                          uint16_t resultLen, void * /*ctx*/) {
    char path[128];
    JsonParser p;
    if (p.getString(argsJson, "path", path, sizeof(path)) != 0) {
        strncpy(result, "missing path", resultLen - 1);
        result[resultLen - 1] = '\0';
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        strncpy(result, "cannot open file", resultLen - 1);
        result[resultLen - 1] = '\0';
        return -1;
    }

    uint16_t n = (uint16_t)fread(result, 1, resultLen - 1, f);
    fclose(f);
    result[n] = '\0';
    return 0;
}

/* -----------------------------------------------------------------------
   write_file
   ----------------------------------------------------------------------- */

static int tool_write_file(const char *argsJson, char *result,
                           uint16_t resultLen, void * /*ctx*/) {
    char path[128];
    char content[512];
    JsonParser p;
    if (p.getString(argsJson, "path",    path,    sizeof(path))    != 0 ||
        p.getString(argsJson, "content", content, sizeof(content)) != 0) {
        strncpy(result, "missing path or content", resultLen - 1);
        result[resultLen - 1] = '\0';
        return -1;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        strncpy(result, "cannot open file for writing", resultLen - 1);
        result[resultLen - 1] = '\0';
        return -1;
    }
    fputs(content, f);
    fclose(f);
    strncpy(result, "ok", resultLen - 1);
    result[resultLen - 1] = '\0';
    return 0;
}

/* -----------------------------------------------------------------------
   list_dir — uses _dos_findfirst / _dos_findnext
   ----------------------------------------------------------------------- */

static int tool_list_dir(const char *argsJson, char *result,
                         uint16_t resultLen, void * /*ctx*/) {
    char path[128];
    JsonParser p;
    if (p.getString(argsJson, "path", path, sizeof(path)) != 0) {
        /* default to current dir */
        strncpy(path, ".", sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    /* Build wildcard pattern: "path\*.*" */
    char pattern[135];
    strncpy(pattern, path, sizeof(pattern) - 5);
    pattern[sizeof(pattern) - 5] = '\0';
    uint16_t plen = (uint16_t)strlen(pattern);
    if (plen > 0 && pattern[plen-1] != '\\' && pattern[plen-1] != '/') {
        pattern[plen++] = '\\';
    }
    pattern[plen++] = '*';
    pattern[plen++] = '.';
    pattern[plen++] = '*';
    pattern[plen]   = '\0';

    struct _find_t fi;
    uint16_t pos = 0;
    int rc = _dos_findfirst(pattern, _A_NORMAL | _A_SUBDIR | _A_RDONLY, &fi);
    while (rc == 0) {
        /* skip . and .. */
        if (fi.name[0] != '.') {
            uint16_t nlen = (uint16_t)strlen(fi.name);
            if (pos + nlen + 2 < resultLen) {
                memcpy(result + pos, fi.name, nlen);
                pos += nlen;
                if (fi.attrib & _A_SUBDIR) result[pos++] = '/';
                result[pos++] = '\n';
            }
        }
        rc = _dos_findnext(&fi);
    }
    result[pos] = '\0';
    if (pos == 0) strncpy(result, "(empty)", resultLen - 1);
    return 0;
}

/* -----------------------------------------------------------------------
   exec_command — popen equivalent; not available on pure DOS, use
   system() + temp file as fallback.
   ----------------------------------------------------------------------- */

static int tool_exec_command(const char *argsJson, char *result,
                             uint16_t resultLen, void * /*ctx*/) {
    char cmd[256];
    JsonParser p;
    if (p.getString(argsJson, "command", cmd, sizeof(cmd)) != 0) {
        strncpy(result, "missing command", resultLen - 1);
        result[resultLen - 1] = '\0';
        return -1;
    }

    /* Redirect output to a temp file */
    char tmp[128] = "C:\\CODY_TMP.TXT";
    char full[300];
    strncpy(full, cmd, sizeof(full) - 30);
    full[sizeof(full) - 30] = '\0';
    strcat(full, " > ");
    strcat(full, tmp);

    system(full);

    FILE *f = fopen(tmp, "r");
    if (!f) {
        strncpy(result, "(no output)", resultLen - 1);
        result[resultLen - 1] = '\0';
        return 0;
    }
    uint16_t n = (uint16_t)fread(result, 1, resultLen - 1, f);
    fclose(f);
    result[n] = '\0';
    remove(tmp);
    return 0;
}

/* -----------------------------------------------------------------------
   get_cwd
   ----------------------------------------------------------------------- */

static int tool_get_cwd(const char * /*argsJson*/, char *result,
                        uint16_t resultLen, void * /*ctx*/) {
    if (getcwd(result, resultLen) == NULL) {
        strncpy(result, ".", resultLen - 1);
        result[resultLen - 1] = '\0';
    }
    return 0;
}

/* -----------------------------------------------------------------------
   toolsInit — register all DOS tools into g_tools[]
   ----------------------------------------------------------------------- */

void toolsInit() {
    g_toolCount = 0;

    static const struct { const char *name; const char *desc; ToolFn fn; } tab[] = {
        { "read_file",     "Read a file. Args: {\"path\":\"...\"}", tool_read_file },
        { "write_file",    "Write a file. Args: {\"path\":\"...\",\"content\":\"...\"}",
                           tool_write_file },
        { "list_dir",      "List directory. Args: {\"path\":\"...\"}",  tool_list_dir },
        { "exec_command",  "Run a DOS command. Args: {\"command\":\"...\"}",
                           tool_exec_command },
        { "get_cwd",       "Get current working directory.",              tool_get_cwd },
    };

    for (uint8_t i = 0; i < 5 && g_toolCount < TOOL_MAX; i++) {
        Tool *t = &g_tools[g_toolCount++];
        strncpy(t->name, tab[i].name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
        strncpy(t->desc, tab[i].desc, sizeof(t->desc) - 1);
        t->desc[sizeof(t->desc) - 1] = '\0';
        t->fn = tab[i].fn;
    }
}
