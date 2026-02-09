#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/wait.h>
#include <json-c/json.h>
#include <limits.h>

#define PORT 8080
#define TEMP_DIR "/tmp/c_converter"
#define BUFFER_SIZE 4096
#define MAX_INCLUDES 512
#define MAX_FILES 512
#define MAX_HEADER_LEN 256
#define MAX_PATH_LEN 1024

#define MAX_SYSTEM_PATHS 8

static char g_system_paths[MAX_SYSTEM_PATHS][MAX_PATH_LEN];
static int g_system_path_count = 0;

void add_system_path(const char *path) {
    if (g_system_path_count >= MAX_SYSTEM_PATHS) return;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    strncpy(g_system_paths[g_system_path_count], path, MAX_PATH_LEN - 1);
    g_system_paths[g_system_path_count][MAX_PATH_LEN - 1] = '\0';
    g_system_path_count++;
}

void init_system_paths(void) {
    add_system_path("/usr/include");
    add_system_path("/usr/local/include");

    FILE *fp = popen("gcc -print-file-name=include 2>/dev/null", "r");
    if (fp) {
        char buf[MAX_PATH_LEN];
        if (fgets(buf, sizeof(buf), fp)) {
            buf[strcspn(buf, "\n")] = '\0';
            add_system_path(buf);
        }
        pclose(fp);
    }
}

typedef struct {
    char *git_url;
    char *repo_name;
    int is_c_project;
    int c_files_count;
    int header_files_count;
    char *header_filename;
    char *error;
    int success;
} ConversionResult;

void free_result(ConversionResult *result);

typedef struct {
    char standard[MAX_INCLUDES][MAX_HEADER_LEN];
    int standard_count;
    char external[MAX_INCLUDES][MAX_HEADER_LEN];
    int external_count;
    char inlined[MAX_FILES][MAX_PATH_LEN];
    int inlined_count;
    char repo_dir[MAX_PATH_LEN];
} ConversionContext;

typedef struct {
    char paths[MAX_FILES][MAX_PATH_LEN];
    int count;
} FileList;

#define MAX_RETRY 10

typedef struct {
    char source[MAX_PATH_LEN];
    int start_line;
    int end_line;
} SourceMapping;

typedef struct {
    SourceMapping entries[MAX_FILES];
    int count;
} LineMap;


int file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

int create_directory(const char *path) {
    return mkdir(path, 0755);
}

void cleanup_directory(const char *path) {
    char command[1024];
    snprintf(command, sizeof(command), "rm -rf \"%s\"", path);
    system(command);
}

char *extract_repo_name(const char *git_url) {
    const char *last_slash = strrchr(git_url, '/');
    if (!last_slash) return NULL;

    const char *name_start = last_slash + 1;
    const char *dot_git = strstr(name_start, ".git");

    if (dot_git) {
        size_t len = (size_t)(dot_git - name_start);
        char *repo_name = malloc(len + 1);
        if (!repo_name) return NULL;
        memcpy(repo_name, name_start, len);
        repo_name[len] = '\0';
        return repo_name;
    } else {
        return strdup(name_start);
    }
}

int clone_repository(const char *git_url, const char *target_dir) {
    char command[1024];
    snprintf(command, sizeof(command), "git clone --depth 1 %s %s 2>/dev/null",
             git_url, target_dir);
    return (system(command) == 0);
}

int is_c_file(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;
    return (strcmp(ext, ".c") == 0);
}

int is_header_file(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;
    return (strcmp(ext, ".h") == 0);
}

void scan_directory(const char *dir_path, int *c_files, int *header_files) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (strcmp(entry->d_name, ".git") == 0)
            continue;

        if (entry->d_type == DT_REG) {
            if (is_c_file(entry->d_name))
                (*c_files)++;
            else if (is_header_file(entry->d_name))
                (*header_files)++;
        } else if (entry->d_type == DT_DIR) {
            char sub_path[MAX_PATH_LEN];
            snprintf(sub_path, sizeof(sub_path), "%s/%s", dir_path, entry->d_name);
            scan_directory(sub_path, c_files, header_files);
        }
    }

    closedir(dir);
}

char *read_file_content(const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) return NULL;

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(file);
        return NULL;
    }

    char *content = malloc((size_t)file_size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(content, 1, (size_t)file_size, file);
    content[read_size] = '\0';
    fclose(file);

    return content;
}


int header_exists_on_system(const char *header) {
    char path[MAX_PATH_LEN];
    struct stat st;
    for (int i = 0; i < g_system_path_count; i++) {
        snprintf(path, sizeof(path), "%s/%s", g_system_paths[i], header);
        if (stat(path, &st) == 0)
            return 1;
    }
    return 0;
}

int include_list_contains(char list[][MAX_HEADER_LEN], int count, const char *item) {
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i], item) == 0)
            return 1;
    }
    return 0;
}

void include_list_add(char list[][MAX_HEADER_LEN], int *count, const char *item) {
    if (*count >= MAX_INCLUDES) return;
    if (include_list_contains(list, *count, item)) return;
    strncpy(list[*count], item, MAX_HEADER_LEN - 1);
    list[*count][MAX_HEADER_LEN - 1] = '\0';
    (*count)++;
}

int is_file_inlined(ConversionContext *ctx, const char *path) {
    for (int i = 0; i < ctx->inlined_count; i++) {
        if (strcmp(ctx->inlined[i], path) == 0)
            return 1;
    }
    return 0;
}

void mark_file_inlined(ConversionContext *ctx, const char *path) {
    if (ctx->inlined_count >= MAX_FILES) return;
    strncpy(ctx->inlined[ctx->inlined_count], path, MAX_PATH_LEN - 1);
    ctx->inlined[ctx->inlined_count][MAX_PATH_LEN - 1] = '\0';
    ctx->inlined_count++;
}

void get_file_directory(const char *filepath, char *dir, size_t dir_size) {
    strncpy(dir, filepath, dir_size - 1);
    dir[dir_size - 1] = '\0';
    char *last_slash = strrchr(dir, '/');
    if (last_slash)
        *last_slash = '\0';
    else
        strncpy(dir, ".", dir_size);
}

int find_header_in_repo(ConversionContext *ctx, const char *include_path,
                        const char *current_dir, char *resolved) {
    char candidate[MAX_PATH_LEN];
    char real[PATH_MAX];

    const char *search_bases[] = {current_dir, ctx->repo_dir, NULL};
    const char *sub_dirs[] = {"include", "src", "lib", NULL};

    for (int i = 0; search_bases[i] != NULL; i++) {
        snprintf(candidate, sizeof(candidate), "%s/%s", search_bases[i], include_path);
        if (file_exists(candidate) && realpath(candidate, real)) {
            strncpy(resolved, real, MAX_PATH_LEN - 1);
            resolved[MAX_PATH_LEN - 1] = '\0';
            return 1;
        }
    }

    for (int i = 0; sub_dirs[i] != NULL; i++) {
        snprintf(candidate, sizeof(candidate), "%s/%s/%s",
                 ctx->repo_dir, sub_dirs[i], include_path);
        if (file_exists(candidate) && realpath(candidate, real)) {
            strncpy(resolved, real, MAX_PATH_LEN - 1);
            resolved[MAX_PATH_LEN - 1] = '\0';
            return 1;
        }
    }

    return 0;
}

int parse_include_line(const char *line, char *header, size_t header_size) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    if (*p != '#') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    if (strncmp(p, "include", 7) != 0) return 0;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;

    char open_char, close_char;
    int type;
    if (*p == '"') {
        open_char = '"'; close_char = '"'; type = 1;
    } else if (*p == '<') {
        open_char = '<'; close_char = '>'; type = 2;
    } else {
        return 0;
    }
    (void)open_char;

    p++;
    const char *end = strchr(p, close_char);
    if (!end) return 0;

    size_t len = (size_t)(end - p);
    if (len >= header_size) len = header_size - 1;
    memcpy(header, p, len);
    header[len] = '\0';
    return type;
}


void process_file_with_context(ConversionContext *ctx, const char *filepath,
                               FILE *output) {
    char *content = read_file_content(filepath);
    if (!content) return;

    char file_dir[MAX_PATH_LEN];
    get_file_directory(filepath, file_dir, sizeof(file_dir));

    char *pos = content;

    while (*pos) {
        char *newline = strchr(pos, '\n');
        size_t line_len = newline ? (size_t)(newline - pos) : strlen(pos);

        char line[4096];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, pos, line_len);
        line[line_len] = '\0';

        char header[MAX_HEADER_LEN];
        int include_type = parse_include_line(line, header, sizeof(header));

        if (include_type == 1) {
            char resolved[MAX_PATH_LEN];
            if (find_header_in_repo(ctx, header, file_dir, resolved)) {
                if (!is_file_inlined(ctx, resolved)) {
                    mark_file_inlined(ctx, resolved);
                    fprintf(output, "\n/* --- Inlined: %s --- */\n", header);
                    process_file_with_context(ctx, resolved, output);
                    fprintf(output, "/* --- End: %s --- */\n\n", header);
                }
            } else {
                if (header_exists_on_system(header))
                    include_list_add(ctx->standard, &ctx->standard_count, header);
                else
                    include_list_add(ctx->external, &ctx->external_count, header);
            }
        } else if (include_type == 2) {
            if (header_exists_on_system(header))
                include_list_add(ctx->standard, &ctx->standard_count, header);
            else
                include_list_add(ctx->external, &ctx->external_count, header);
        } else {
            fprintf(output, "%s\n", line);
        }

        pos = newline ? newline + 1 : pos + line_len;
    }

    free(content);
}

void collect_source_files(const char *dir_path, FileList *c_files,
                          FileList *h_files) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (strcmp(entry->d_name, ".git") == 0)
            continue;

        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        if (entry->d_type == DT_REG) {
            char real[PATH_MAX];
            if (is_c_file(entry->d_name) && c_files->count < MAX_FILES) {
                if (realpath(path, real)) {
                    strncpy(c_files->paths[c_files->count], real, MAX_PATH_LEN - 1);
                    c_files->paths[c_files->count][MAX_PATH_LEN - 1] = '\0';
                    c_files->count++;
                }
            } else if (is_header_file(entry->d_name) && h_files->count < MAX_FILES) {
                if (realpath(path, real)) {
                    strncpy(h_files->paths[h_files->count], real, MAX_PATH_LEN - 1);
                    h_files->paths[h_files->count][MAX_PATH_LEN - 1] = '\0';
                    h_files->count++;
                }
            }
        } else if (entry->d_type == DT_DIR) {
            collect_source_files(path, c_files, h_files);
        }
    }

    closedir(dir);
}

void remove_from_filelist(FileList *list, const char *path) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->paths[i], path) == 0) {
            for (int j = i; j < list->count - 1; j++)
                strncpy(list->paths[j], list->paths[j + 1], MAX_PATH_LEN);
            list->count--;
            return;
        }
    }
}

int try_compile(const char *header_path, char *error_buf, size_t error_size) {
    char command[MAX_PATH_LEN + 64];
    snprintf(command, sizeof(command),
             "gcc -fsyntax-only -x c \"%s\" 2>&1", header_path);

    FILE *fp = popen(command, "r");
    if (!fp) {
        error_buf[0] = '\0';
        return -1;
    }

    size_t total = 0;
    char buf[512];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t len = strlen(buf);
        if (total + len < error_size - 1) {
            memcpy(error_buf + total, buf, len);
            total += len;
        }
    }
    error_buf[total] = '\0';

    int status = pclose(fp);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int find_conflicting_source(const char *error_output, LineMap *map,
                            char *source_path, size_t path_size) {
    const char *patterns[] = {"redefinition of", "conflicting types", NULL};
    const char *found = NULL;

    for (int i = 0; patterns[i]; i++) {
        found = strstr(error_output, patterns[i]);
        if (found) break;
    }
    if (!found) return 0;

    /* Walk backwards from the pattern match to find the line number.
       gcc format: "file.h:LINE:COL: error: ..." */
    const char *line_start = found;
    while (line_start > error_output && *(line_start - 1) != '\n')
        line_start--;

    /* Extract line number: skip filename, then parse number after first colon */
    const char *first_colon = strchr(line_start, ':');
    if (!first_colon) return 0;
    int error_line = atoi(first_colon + 1);
    if (error_line <= 0) return 0;

    /* Look up which source file owns this line */
    for (int i = 0; i < map->count; i++) {
        if (error_line >= map->entries[i].start_line &&
            error_line <= map->entries[i].end_line) {
            strncpy(source_path, map->entries[i].source, path_size - 1);
            source_path[path_size - 1] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Strategy 1: Parse build system files to find library sources */
FileList filter_by_build_system(const char *repo_dir, FileList *all_c) {
    FileList result;
    memset(&result, 0, sizeof(result));

    char path[MAX_PATH_LEN];
    char *content = NULL;

    /* Try CMakeLists.txt */
    snprintf(path, sizeof(path), "%s/CMakeLists.txt", repo_dir);
    content = read_file_content(path);
    if (content) {
        /* Look for add_library(name file1.c file2.c ...) */
        char *p = content;
        while ((p = strstr(p, "add_library")) != NULL) {
            char *paren = strchr(p, '(');
            if (!paren) { p++; continue; }
            char *close = strchr(paren, ')');
            if (!close) { p++; continue; }

            /* Skip library name (first token after paren) */
            char *tok = paren + 1;
            while (*tok == ' ' || *tok == '\t' || *tok == '\n') tok++;
            while (*tok && *tok != ' ' && *tok != '\t' && *tok != '\n' && *tok != ')') tok++;

            /* Extract .c filenames */
            while (tok < close) {
                while (*tok == ' ' || *tok == '\t' || *tok == '\n') tok++;
                if (tok >= close) break;
                char *end = tok;
                while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != ')') end++;
                size_t len = (size_t)(end - tok);
                char fname[MAX_PATH_LEN];
                if (len > 0 && len < sizeof(fname)) {
                    memcpy(fname, tok, len);
                    fname[len] = '\0';

                    /* Skip keywords like STATIC, SHARED, MODULE */
                    if (strcmp(fname, "STATIC") != 0 &&
                        strcmp(fname, "SHARED") != 0 &&
                        strcmp(fname, "MODULE") != 0 &&
                        strcmp(fname, "OBJECT") != 0) {
                        /* Check if this matches a .c file */
                        const char *ext = strrchr(fname, '.');
                        if (ext && strcmp(ext, ".c") == 0) {
                            /* Get the basename */
                            const char *base = strrchr(fname, '/');
                            base = base ? base + 1 : fname;

                            for (int i = 0; i < all_c->count; i++) {
                                const char *cbase = strrchr(all_c->paths[i], '/');
                                cbase = cbase ? cbase + 1 : all_c->paths[i];
                                if (strcmp(cbase, base) == 0 && result.count < MAX_FILES) {
                                    strncpy(result.paths[result.count], all_c->paths[i], MAX_PATH_LEN - 1);
                                    result.count++;
                                }
                            }
                        }
                    }
                }
                tok = end;
            }
            p = close + 1;
        }
        free(content);
        if (result.count > 0) return result;
    }

    /* Try Makefile / makefile */
    const char *makefiles[] = {"Makefile", "makefile", NULL};
    for (int m = 0; makefiles[m]; m++) {
        snprintf(path, sizeof(path), "%s/%s", repo_dir, makefiles[m]);
        content = read_file_content(path);
        if (!content) continue;

        /* Look for SRCS=, SOURCES=, SRC=, OBJS= lines */
        const char *prefixes[] = {"SRCS", "SOURCES", "SRC", "OBJS", NULL};
        char *line = content;
        while (*line) {
            char *nl = strchr(line, '\n');
            size_t llen = nl ? (size_t)(nl - line) : strlen(line);
            char lbuf[4096];
            if (llen >= sizeof(lbuf)) llen = sizeof(lbuf) - 1;
            memcpy(lbuf, line, llen);
            lbuf[llen] = '\0';

            for (int pi = 0; prefixes[pi]; pi++) {
                char *var = strstr(lbuf, prefixes[pi]);
                if (!var) continue;
                /* Skip to after '=' */
                char *eq = strchr(var, '=');
                if (!eq) continue;
                eq++;
                while (*eq == ' ' || *eq == '\t') eq++;

                /* Extract filenames from this line */
                char *tok = eq;
                while (*tok) {
                    while (*tok == ' ' || *tok == '\t' || *tok == '\\') tok++;
                    if (!*tok) break;
                    char *end = tok;
                    while (*end && *end != ' ' && *end != '\t' && *end != '\\') end++;
                    size_t tlen = (size_t)(end - tok);
                    char fname[MAX_PATH_LEN];
                    if (tlen > 0 && tlen < sizeof(fname)) {
                        memcpy(fname, tok, tlen);
                        fname[tlen] = '\0';

                        /* Convert .o to .c */
                        char *ext = strrchr(fname, '.');
                        if (ext && strcmp(ext, ".o") == 0) {
                            ext[1] = 'c';
                            ext[2] = '\0';
                        }

                        if (ext && (strcmp(strrchr(fname, '.'), ".c") == 0)) {
                            const char *base = strrchr(fname, '/');
                            base = base ? base + 1 : fname;
                            for (int i = 0; i < all_c->count; i++) {
                                const char *cbase = strrchr(all_c->paths[i], '/');
                                cbase = cbase ? cbase + 1 : all_c->paths[i];
                                if (strcmp(cbase, base) == 0 && result.count < MAX_FILES) {
                                    /* Avoid duplicates */
                                    int dup = 0;
                                    for (int d = 0; d < result.count; d++) {
                                        if (strcmp(result.paths[d], all_c->paths[i]) == 0) {
                                            dup = 1; break;
                                        }
                                    }
                                    if (!dup) {
                                        strncpy(result.paths[result.count], all_c->paths[i], MAX_PATH_LEN - 1);
                                        result.count++;
                                    }
                                }
                            }
                        }
                    }
                    tok = end;
                }
            }
            line = nl ? nl + 1 : line + llen;
        }
        free(content);
        if (result.count > 0) return result;
    }

    /* Try meson.build */
    snprintf(path, sizeof(path), "%s/meson.build", repo_dir);
    content = read_file_content(path);
    if (content) {
        const char *lib_funcs[] = {"library(", "static_library(", "shared_library(", NULL};
        for (int lf = 0; lib_funcs[lf]; lf++) {
            char *p = content;
            while ((p = strstr(p, lib_funcs[lf])) != NULL) {
                char *paren = strchr(p, '(');
                if (!paren) { p++; continue; }

                /* Find matching close paren (simple, not nested) */
                char *close = strchr(paren, ')');
                if (!close) { p++; continue; }

                /* Extract quoted .c filenames */
                char *s = paren;
                while (s < close) {
                    char *q = strchr(s, '\'');
                    if (!q || q >= close) break;
                    char *q2 = strchr(q + 1, '\'');
                    if (!q2 || q2 >= close) break;

                    size_t slen = (size_t)(q2 - q - 1);
                    char fname[MAX_PATH_LEN];
                    if (slen > 0 && slen < sizeof(fname)) {
                        memcpy(fname, q + 1, slen);
                        fname[slen] = '\0';

                        const char *ext = strrchr(fname, '.');
                        if (ext && strcmp(ext, ".c") == 0) {
                            const char *base = strrchr(fname, '/');
                            base = base ? base + 1 : fname;
                            for (int i = 0; i < all_c->count; i++) {
                                const char *cbase = strrchr(all_c->paths[i], '/');
                                cbase = cbase ? cbase + 1 : all_c->paths[i];
                                if (strcmp(cbase, base) == 0 && result.count < MAX_FILES) {
                                    strncpy(result.paths[result.count], all_c->paths[i], MAX_PATH_LEN - 1);
                                    result.count++;
                                }
                            }
                        }
                    }
                    s = q2 + 1;
                }
                p = close + 1;
            }
        }
        free(content);
        if (result.count > 0) return result;
    }

    return result;
}

/* Strategy 2: Match .c files to .h files by basename */
FileList filter_by_header_match(FileList *c_files, FileList *h_files) {
    FileList result;
    memset(&result, 0, sizeof(result));

    for (int i = 0; i < c_files->count; i++) {
        const char *c_base = strrchr(c_files->paths[i], '/');
        c_base = c_base ? c_base + 1 : c_files->paths[i];

        /* Get stem (basename without extension) */
        char c_stem[MAX_PATH_LEN];
        strncpy(c_stem, c_base, sizeof(c_stem) - 1);
        c_stem[sizeof(c_stem) - 1] = '\0';
        char *dot = strrchr(c_stem, '.');
        if (dot) *dot = '\0';

        for (int j = 0; j < h_files->count; j++) {
            const char *h_base = strrchr(h_files->paths[j], '/');
            h_base = h_base ? h_base + 1 : h_files->paths[j];

            char h_stem[MAX_PATH_LEN];
            strncpy(h_stem, h_base, sizeof(h_stem) - 1);
            h_stem[sizeof(h_stem) - 1] = '\0';
            char *hdot = strrchr(h_stem, '.');
            if (hdot) *hdot = '\0';

            if (strcmp(c_stem, h_stem) == 0 && result.count < MAX_FILES) {
                strncpy(result.paths[result.count], c_files->paths[i], MAX_PATH_LEN - 1);
                result.count++;
                break;
            }
        }
    }

    return result;
}

/* Check if a main() definition at position p in content is inside an #if/#ifdef block.
   Simple heuristic: count #if/#ifdef vs #endif lines before the position. */
int main_is_ifdef_guarded(const char *content, const char *main_pos) {
    int depth = 0;
    const char *line = content;
    while (line < main_pos) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#') {
            const char *dir = line + 1;
            while (*dir == ' ' || *dir == '\t') dir++;
            if (strncmp(dir, "if", 2) == 0) depth++;
            else if (strncmp(dir, "endif", 5) == 0 && depth > 0) depth--;
        }
        const char *nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }
    return depth > 0;
}

/* Remove .c files that define an unconditional main() */
void strip_main_files(FileList *list) {
    int i = 0;
    while (i < list->count) {
        char *content = read_file_content(list->paths[i]);
        if (!content) { i++; continue; }

        int has_unguarded_main = 0;
        char *p = content;
        while ((p = strstr(p, "main")) != NULL) {
            /* Check it looks like a function definition: main\s*( */
            char *after = p + 4;
            while (*after == ' ' || *after == '\t') after++;
            if (*after == '(') {
                /* Check preceding char is plausible for a definition */
                if (p == content || *(p-1) == '\n' || *(p-1) == ' ' ||
                    *(p-1) == '\t' || *(p-1) == '*') {
                    if (!main_is_ifdef_guarded(content, p)) {
                        has_unguarded_main = 1;
                        break;
                    }
                }
            }
            p = after;
        }
        free(content);

        if (has_unguarded_main) {
            printf("excluded (has main): %s\n", list->paths[i]);
            remove_from_filelist(list, list->paths[i]);
        } else {
            i++;
        }
    }
}

void make_guard_name(const char *repo_name, char *guard, size_t guard_size) {
    size_t j = 0;
    for (size_t i = 0; repo_name[i] && j < guard_size - 1; i++) {
        char c = repo_name[i];
        if (c >= 'a' && c <= 'z')
            guard[j++] = c - 32;
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            guard[j++] = c;
        else
            guard[j++] = '_';
    }
    guard[j] = '\0';
}

/* Generate header content from given file lists.
 * Returns malloc'd string with the full header content.
 * If line_map is non-NULL, populates it with source file -> line range mappings.
 * If sweep_remaining_headers is set, also includes .h files not yet inlined. */
char *generate_header_content(const char *repo_dir, const char *repo_name,
                              FileList *c_files, FileList *h_files,
                              LineMap *line_map, int sweep_remaining_headers) {
    ConversionContext *ctx = calloc(1, sizeof(ConversionContext));
    if (!ctx) return NULL;
    strncpy(ctx->repo_dir, repo_dir, MAX_PATH_LEN - 1);

    /* Generate the code body into a memstream so we can track line numbers */
    char *code_buf = NULL;
    size_t code_size = 0;
    FILE *code_stream = open_memstream(&code_buf, &code_size);
    if (!code_stream) { free(ctx); return NULL; }

    /* We'll track lines per source file for the line map.
     * We count lines in the code body (not the final output header/includes). */
    int current_line = 1; /* line counter within the code body */

    for (int i = 0; i < c_files->count; i++) {
        if (is_file_inlined(ctx, c_files->paths[i]))
            continue;
        mark_file_inlined(ctx, c_files->paths[i]);

        const char *rel = c_files->paths[i] + strlen(repo_dir);
        if (*rel == '/') rel++;
        fprintf(code_stream, "\n/* %s */\n", rel);
        current_line += 2; /* blank line + comment line */

        int start_line = current_line;

        /* Count lines written by process_file_with_context */
        long pos_before = (long)code_size;
        fflush(code_stream);
        pos_before = (long)code_size;
        process_file_with_context(ctx, c_files->paths[i], code_stream);
        fflush(code_stream);
        long pos_after = (long)code_size;

        /* Count newlines in what was just written */
        int lines_written = 0;
        for (long p = pos_before; p < pos_after; p++) {
            if (code_buf[p] == '\n') lines_written++;
        }
        current_line += lines_written;

        if (line_map && line_map->count < MAX_FILES) {
            SourceMapping *sm = &line_map->entries[line_map->count];
            strncpy(sm->source, c_files->paths[i], MAX_PATH_LEN - 1);
            sm->source[MAX_PATH_LEN - 1] = '\0';
            sm->start_line = start_line;
            sm->end_line = current_line - 1;
            line_map->count++;
        }
    }

    if (sweep_remaining_headers) {
        for (int i = 0; i < h_files->count; i++) {
            if (is_file_inlined(ctx, h_files->paths[i]))
                continue;

            /* Skip headers in test/example directories */
            const char *rel = h_files->paths[i] + strlen(repo_dir);
            if (*rel == '/') rel++;
            if (strncmp(rel, "test/", 5) == 0 || strncmp(rel, "tests/", 6) == 0 ||
                strncmp(rel, "example/", 8) == 0 || strncmp(rel, "examples/", 9) == 0 ||
                strncmp(rel, "bench/", 6) == 0 || strncmp(rel, "benchmark/", 10) == 0 ||
                strstr(rel, "/test/") || strstr(rel, "/tests/") ||
                strstr(rel, "/example/") || strstr(rel, "/examples/"))
                continue;

            mark_file_inlined(ctx, h_files->paths[i]);
            fprintf(code_stream, "\n/* %s */\n", rel);
            current_line += 2;
            process_file_with_context(ctx, h_files->paths[i], code_stream);
            fflush(code_stream);
            /* No need to track these in line_map - they're headers, not conflict sources */
        }
    }

    fclose(code_stream);

    /* Assemble the final header: guard + includes + code body + end guard */
    char guard[256];
    make_guard_name(repo_name, guard, sizeof(guard));

    char *result_buf = NULL;
    size_t result_size = 0;
    FILE *result_stream = open_memstream(&result_buf, &result_size);
    if (!result_stream) { free(code_buf); free(ctx); return NULL; }

    fprintf(result_stream, "#ifndef %s_COMBINED_H\n", guard);
    fprintf(result_stream, "#define %s_COMBINED_H\n\n", guard);
    fprintf(result_stream, "/*\n * Auto-generated header-only file\n"
                           " * Repository: %s\n */\n\n", repo_name);

    /* Count how many lines the header preamble takes:
       #ifndef, #define, blank, comment-open, comment-body, comment-close+blank */
    int preamble_lines = 6;

    if (ctx->standard_count > 0) {
        for (int i = 0; i < ctx->standard_count; i++) {
            fprintf(result_stream, "#include <%s>\n", ctx->standard[i]);
            preamble_lines++;
        }
        fprintf(result_stream, "\n");
        preamble_lines++;
    }

    if (ctx->external_count > 0) {
        for (int i = 0; i < ctx->external_count; i++) {
            fprintf(result_stream, "#include <%s>\n", ctx->external[i]);
            preamble_lines++;
        }
        fprintf(result_stream, "\n");
        preamble_lines++;
    }

    /* Adjust line_map entries to account for preamble lines */
    if (line_map) {
        for (int i = 0; i < line_map->count; i++) {
            line_map->entries[i].start_line += preamble_lines;
            line_map->entries[i].end_line += preamble_lines;
        }
    }

    if (code_buf)
        fwrite(code_buf, 1, code_size, result_stream);

    fprintf(result_stream, "\n#endif /* %s_COMBINED_H */\n", guard);

    fclose(result_stream);
    free(code_buf);
    free(ctx);

    return result_buf;
}

char *create_header_only_file(const char *repo_dir, const char *repo_name) {
    FileList *c_files = calloc(1, sizeof(FileList));
    FileList *h_files = calloc(1, sizeof(FileList));
    if (!c_files || !h_files) {
        free(c_files); free(h_files);
        return NULL;
    }

    collect_source_files(repo_dir, c_files, h_files);
    strip_main_files(c_files);

    char *content = NULL;

    /* Strategy 1: Try build system parsing */
    FileList filtered = filter_by_build_system(repo_dir, c_files);
    if (filtered.count > 0) {
        printf("strategy: build system (%d files)\n", filtered.count);
        content = generate_header_content(repo_dir, repo_name,
                                          &filtered, h_files, NULL, 0);
        goto write_output;
    }

    /* Strategy 2: Try header-name matching */
    filtered = filter_by_header_match(c_files, h_files);
    if (filtered.count > 0) {
        printf("strategy: header match (%d files)\n", filtered.count);
        content = generate_header_content(repo_dir, repo_name,
                                          &filtered, h_files, NULL, 0);
        goto write_output;
    }

    /* Strategy 3: Compile feedback loop */
    printf("strategy: compile feedback\n");
    {
        char temp_path[MAX_PATH_LEN];
        snprintf(temp_path, sizeof(temp_path), "%s/.giga_test.h", TEMP_DIR);

        for (int retry = 0; retry < MAX_RETRY; retry++) {
            LineMap lmap;
            memset(&lmap, 0, sizeof(lmap));

            free(content);
            content = generate_header_content(repo_dir, repo_name,
                                              c_files, h_files, &lmap, 1);
            if (!content) break;

            /* Write to temp file for compilation test */
            FILE *tmp = fopen(temp_path, "w");
            if (!tmp) break;
            fputs(content, tmp);
            fclose(tmp);

            char errors[4096];
            int rc = try_compile(temp_path, errors, sizeof(errors));
            remove(temp_path);

            if (rc == 0) break; /* Clean compile */

            char bad_source[MAX_PATH_LEN];
            if (!find_conflicting_source(errors, &lmap, bad_source, sizeof(bad_source)))
                break; /* Can't identify the problem */

            remove_from_filelist(c_files, bad_source);
            printf("removed: %s\n", bad_source);

            if (c_files->count == 0) break;
        }
    }

write_output:
    if (!content) {
        free(c_files); free(h_files);
        return NULL;
    }

    char header_filename[256];
    snprintf(header_filename, sizeof(header_filename), "%s_combined.h", repo_name);

    char header_path[512];
    snprintf(header_path, sizeof(header_path), "%s/%s", TEMP_DIR, header_filename);

    FILE *output = fopen(header_path, "w");
    if (!output) {
        free(content); free(c_files); free(h_files);
        return NULL;
    }

    fputs(content, output);
    fclose(output);
    free(content);
    free(c_files);
    free(h_files);

    return strdup(header_filename);
}


ConversionResult *convert_git_repository(const char *git_url) {
    ConversionResult *result = calloc(1, sizeof(ConversionResult));
    if (!result) return NULL;

    result->git_url = strdup(git_url);
    result->repo_name = extract_repo_name(git_url);

    if (!result->repo_name) {
        result->error = strdup("Invalid repository URL");
        return result;
    }

    create_directory(TEMP_DIR);

    char repo_dir[512];
    snprintf(repo_dir, sizeof(repo_dir), "%s/%s", TEMP_DIR, result->repo_name);
    cleanup_directory(repo_dir);

    printf("Cloning repository: %s\n", git_url);
    if (!clone_repository(git_url, repo_dir)) {
        result->error = strdup("Failed to clone repository");
        cleanup_directory(repo_dir);
        return result;
    }

    printf("Scanning for C files...\n");
    int c_files = 0, header_files = 0;
    scan_directory(repo_dir, &c_files, &header_files);

    result->c_files_count = c_files;
    result->header_files_count = header_files;
    result->is_c_project = (c_files > 0);

    printf("Found %d C files and %d header files\n", c_files, header_files);

    if (!result->is_c_project) {
        result->error = strdup("No C files found in repository");
        cleanup_directory(repo_dir);
        return result;
    }

    printf("Creating header-only file...\n");
    result->header_filename = create_header_only_file(repo_dir, result->repo_name);

    if (!result->header_filename) {
        result->error = strdup("Failed to create header-only file");
        cleanup_directory(repo_dir);
        return result;
    }

    cleanup_directory(repo_dir);
    result->success = 1;

    printf("Conversion completed successfully!\n");
    return result;
}


json_object *create_json_response(ConversionResult *result) {
    json_object *response = json_object_new_object();

    json_object_object_add(response, "success",
                           json_object_new_boolean(result->success));

    if (result->success) {
        json_object_object_add(response, "repository",
                               json_object_new_string(result->repo_name));
        json_object_object_add(response, "c_files_count",
                               json_object_new_int(result->c_files_count));
        json_object_object_add(response, "header_files_count",
                               json_object_new_int(result->header_files_count));
        json_object_object_add(response, "filename",
                               json_object_new_string(result->header_filename));
    } else {
        json_object_object_add(response, "error",
                               json_object_new_string(
                                   result->error ? result->error : "Unknown error"));
    }

    return response;
}

char *read_html_file(void) {
    return read_file_content("index.html");
}

void send_response(int client_fd, const char *content,
                   const char *content_type, int status_code) {
    char response_header[1024];
    snprintf(response_header, sizeof(response_header),
             "HTTP/1.1 %d OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
             "Access-Control-Allow-Headers: Content-Type\r\n"
             "\r\n",
             status_code, content_type, strlen(content));

    write(client_fd, response_header, strlen(response_header));
    write(client_fd, content, strlen(content));
}

void handle_request(int client_fd, const char *method,
                    const char *url, const char *body) {
    if (strcmp(method, "GET") == 0 && strcmp(url, "/") == 0) {
        char *html_content = read_html_file();
        if (html_content) {
            send_response(client_fd, html_content, "text/html", 200);
            free(html_content);
        } else {
            const char *err = "<html><body><h1>Giga-Header</h1>"
                              "<p>Error loading page</p></body></html>";
            send_response(client_fd, err, "text/html", 500);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(url, "/convert") == 0) {
        printf("Received conversion request\n");

        json_object *request_json = json_tokener_parse(body);
        if (!request_json) {
            const char *e = "{\"success\":false,\"error\":\"Invalid JSON\"}";
            send_response(client_fd, e, "application/json", 400);
            return;
        }

        json_object *git_url_obj;
        if (!json_object_object_get_ex(request_json, "git_url", &git_url_obj)) {
            const char *e = "{\"success\":false,\"error\":\"Missing git_url field\"}";
            send_response(client_fd, e, "application/json", 400);
            json_object_put(request_json);
            return;
        }

        const char *git_url = json_object_get_string(git_url_obj);
        printf("Processing URL: %s\n", git_url);

        ConversionResult *result = convert_git_repository(git_url);

        json_object *response_json = create_json_response(result);
        const char *response_string = json_object_to_json_string(response_json);

        printf("Sending response: %s\n", response_string);
        send_response(client_fd, response_string, "application/json", 200);

        json_object_put(response_json);
        json_object_put(request_json);

        free_result(result);
    } else {
        const char *e = "{\"success\":false,\"error\":\"Not found\"}";
        send_response(client_fd, e, "application/json", 404);
    }
}

void free_result(ConversionResult *result) {
    if (!result) return;
    free(result->git_url);
    free(result->repo_name);
    free(result->header_filename);
    free(result->error);
    free(result);
}

int run_cli(const char *git_url, const char *output_path) {
    ConversionResult *result = convert_git_repository(git_url);

    if (!result || !result->success) {
        fprintf(stderr, "error: %s\n",
                result && result->error ? result->error : "unknown error");
        free_result(result);
        return 1;
    }

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/%s", TEMP_DIR, result->header_filename);

    char *content = read_file_content(src_path);
    remove(src_path);

    if (!content) {
        fprintf(stderr, "error: could not read generated file\n");
        free_result(result);
        return 1;
    }

    const char *dest = output_path ? output_path : result->header_filename;

    FILE *out = fopen(dest, "w");
    if (!out) {
        fprintf(stderr, "error: could not write to %s\n", dest);
        free(content);
        free_result(result);
        return 1;
    }
    fputs(content, out);
    fclose(out);
    free(content);

    printf("repo:    %s\n", result->repo_name);
    printf("c files: %d\n", result->c_files_count);
    printf("h files: %d\n", result->header_files_count);
    printf("output:  %s\n", dest);

    free_result(result);
    return 0;
}

int run_server(void) {
    int server_fd, client_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};

    create_directory(TEMP_DIR);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        return 1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   &opt, sizeof(opt))) {
        perror("setsockopt");
        return 1;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return 1;
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen");
        return 1;
    }

    printf("Giga-Header Server running on port %d\n", PORT);
    printf("Open http://localhost:%d in your browser\n", PORT);
    printf("Press Ctrl+C to stop the server...\n");

    while (1) {
        if ((client_fd = accept(server_fd, (struct sockaddr *)&address,
                                &addrlen)) < 0) {
            perror("accept");
            continue;
        }

        read(client_fd, buffer, BUFFER_SIZE);

        char method[16], url[256], version[16];
        sscanf(buffer, "%15s %255s %15s", method, url, version);

        char *body = strstr(buffer, "\r\n\r\n");
        if (body) body += 4;

        handle_request(client_fd, method, url, body ? body : "");

        close(client_fd);
        memset(buffer, 0, BUFFER_SIZE);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    init_system_paths();

    if (argc < 2) {
        printf("usage: %s <git_url> [-o output.h]\n"
               "       %s serve\n", argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "serve") == 0)
        return run_server();

    const char *git_url = argv[1];
    const char *output_path = NULL;

    if (argc >= 4 && strcmp(argv[2], "-o") == 0)
        output_path = argv[3];

    return run_cli(git_url, output_path);
}
