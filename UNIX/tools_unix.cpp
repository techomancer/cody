#include "tools.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
   Tool implementations (POSIX)
   ------------------------------------------------------------------------- */

static int tool_read_file(const char *argsJson, char *result,
                          uint16_t resultLen, void * /*ctx*/) {
    JsonParser p;
    char filename[256];
    if (p.getString(argsJson, "filename", filename, sizeof(filename)) != 0) {
        snprintf(result, resultLen, "error: missing 'filename' argument");
        return -1;
    }
    FILE *f = fopen(filename, "r");
    if (!f) {
        snprintf(result, resultLen, "error: cannot open '%s'", filename);
        return -1;
    }
    uint16_t n = (uint16_t)fread(result, 1, resultLen - 1, f);
    fclose(f);
    result[n] = '\0';
    return 0;
}

static int tool_write_file(const char *argsJson, char *result,
                           uint16_t resultLen, void * /*ctx*/) {
    JsonParser p;
    char filename[256];
    char content[TOOL_RESULT_LEN];
    if (p.getString(argsJson, "filename", filename, sizeof(filename)) != 0) {
        snprintf(result, resultLen, "error: missing 'filename'");
        return -1;
    }
    if (p.getString(argsJson, "content", content, sizeof(content)) != 0) {
        snprintf(result, resultLen, "error: missing 'content'");
        return -1;
    }
    FILE *f = fopen(filename, "w");
    if (!f) {
        snprintf(result, resultLen, "error: cannot write '%s'", filename);
        return -1;
    }
    fputs(content, f);
    fclose(f);
    snprintf(result, resultLen, "ok");
    return 0;
}

static int tool_list_dir(const char *argsJson, char *result,
                         uint16_t resultLen, void * /*ctx*/) {
    JsonParser p;
    char path[256];
    if (p.getString(argsJson, "path", path, sizeof(path)) != 0)
        strcpy(path, ".");

    DIR *d = opendir(path);
    if (!d) {
        snprintf(result, resultLen, "error: cannot open dir '%s'", path);
        return -1;
    }
    uint16_t pos = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && pos + 2 < resultLen) {
        if (ent->d_name[0] == '.') continue; /* skip . and .. */
        uint16_t nlen = (uint16_t)strlen(ent->d_name);
        if (pos + nlen + 1 >= resultLen) break;
        memcpy(result + pos, ent->d_name, nlen);
        pos += nlen;
        result[pos++] = '\n';
    }
    closedir(d);
    result[pos] = '\0';
    return 0;
}

static int tool_exec_command(const char *argsJson, char *result,
                             uint16_t resultLen, void * /*ctx*/) {
    JsonParser p;
    char cmd[256];
    if (p.getString(argsJson, "command", cmd, sizeof(cmd)) != 0) {
        snprintf(result, resultLen, "error: missing 'command'");
        return -1;
    }
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        snprintf(result, resultLen, "error: popen failed");
        return -1;
    }
    uint16_t n = (uint16_t)fread(result, 1, resultLen - 1, pipe);
    pclose(pipe);
    result[n] = '\0';
    return 0;
}

static int tool_get_cwd(const char * /*argsJson*/, char *result,
                        uint16_t resultLen, void * /*ctx*/) {
    if (getcwd(result, resultLen) == NULL) {
        snprintf(result, resultLen, "error: getcwd failed");
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
   Registration
   ------------------------------------------------------------------------- */

static void registerTool(const char *name, const char *desc, ToolFn fn) {
    if (g_toolCount >= TOOL_MAX) return;
    Tool &t = g_tools[g_toolCount++];
    strncpy(t.name, name, TOOL_NAME_LEN - 1);
    t.name[TOOL_NAME_LEN - 1] = '\0';
    strncpy(t.desc, desc, TOOL_DESC_LEN - 1);
    t.desc[TOOL_DESC_LEN - 1] = '\0';
    t.fn = fn;
}

void toolsInit() {
    g_toolCount = 0;
    registerTool("read_file",
        "Read a file and return its contents. Args: {\"filename\":\"path\"}",
        tool_read_file);
    registerTool("write_file",
        "Write content to a file. Args: {\"filename\":\"path\",\"content\":\"text\"}",
        tool_write_file);
    registerTool("list_dir",
        "List files in a directory. Args: {\"path\":\".\"}",
        tool_list_dir);
    registerTool("exec_command",
        "Execute a shell command and return stdout. Args: {\"command\":\"cmd\"}",
        tool_exec_command);
    registerTool("get_cwd",
        "Return the current working directory. Args: {}",
        tool_get_cwd);
}
