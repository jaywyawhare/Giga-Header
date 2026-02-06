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
#include <json-c/json.h>

#define PORT 8080
#define TEMP_DIR "/tmp/c_converter"
#define BUFFER_SIZE 4096

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

int file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0);
}

int create_directory(const char *path) {
    return mkdir(path, 0755);
}

void cleanup_directory(const char *path) {
    char command[1024];
    snprintf(command, sizeof(command), "rm -rf \"%s\"", path);
    system(command);
}

char* extract_repo_name(const char *git_url) {
    const char *last_slash = strrchr(git_url, '/');
    if (!last_slash) return NULL;
    
    const char *name_start = last_slash + 1;
    const char *dot_git = strstr(name_start, ".git");
    
    if (dot_git) {
        size_t len = dot_git - name_start;
        char *repo_name = malloc(len + 1);
        strncpy(repo_name, name_start, len);
        repo_name[len] = '\0';
        return repo_name;
    } else {
        return strdup(name_start);
    }
}

int clone_repository(const char *git_url, const char *target_dir) {
    char command[1024];
    snprintf(command, sizeof(command), "git clone %s %s 2>/dev/null", git_url, target_dir);
    
    int result = system(command);
    return (result == 0);
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
    DIR *dir;
    struct dirent *entry;
    
    dir = opendir(dir_path);
    if (!dir) return;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            if (is_c_file(entry->d_name)) {
                (*c_files)++;
            } else if (is_header_file(entry->d_name)) {
                (*header_files)++;
            }
        } else if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char sub_path[1024];
            snprintf(sub_path, sizeof(sub_path), "%s/%s", dir_path, entry->d_name);
            scan_directory(sub_path, c_files, header_files);
        }
    }
    
    closedir(dir);
}

char* read_file_content(const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) return NULL;
    
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char *content = malloc(file_size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }
    
    fread(content, 1, file_size, file);
    content[file_size] = '\0';
    fclose(file);
    
    return content;
}

void process_c_file(const char *filepath, FILE *output_header) {
    char *content = read_file_content(filepath);
    if (!content) return;
    
    fprintf(output_header, "// === File: %s ===\n", filepath);
    
    char *line = strtok(content, "\n");
    while (line != NULL) {
        if (strncmp(line, "#include", 8) != 0) {
            fprintf(output_header, "%s\n", line);
        }
        line = strtok(NULL, "\n");
    }
    
    fprintf(output_header, "\n");
    free(content);
}

void process_directory(const char *dir_path, FILE *output_header) {
    DIR *dir;
    struct dirent *entry;
    
    dir = opendir(dir_path);
    if (!dir) return;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG && is_c_file(entry->d_name)) {
            char filepath[1024];
            snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, entry->d_name);
            process_c_file(filepath, output_header);
        } else if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char sub_path[1024];
            snprintf(sub_path, sizeof(sub_path), "%s/%s", dir_path, entry->d_name);
            process_directory(sub_path, output_header);
        }
    }
    
    closedir(dir);
}

char* create_header_only_file(const char *repo_dir, const char *repo_name) {
    char header_filename[256];
    snprintf(header_filename, sizeof(header_filename), "%s_combined.h", repo_name);
    
    char header_path[512];
    snprintf(header_path, sizeof(header_path), "%s/%s", TEMP_DIR, header_filename);
    
    FILE *header_file = fopen(header_path, "w");
    if (!header_file) return NULL;
    
    fprintf(header_file, "#ifndef %s_COMBINED_H\n", repo_name);
    fprintf(header_file, "#define %s_COMBINED_H\n\n", repo_name);
    
    fprintf(header_file, "// Auto-generated header-only file from C project\n");
    fprintf(header_file, "// Repository: %s\n\n", repo_name);
    
    fprintf(header_file, "#include <stdio.h>\n");
    fprintf(header_file, "#include <stdlib.h>\n");
    fprintf(header_file, "#include <string.h>\n");
    fprintf(header_file, "#include <stdint.h>\n\n");
    
    process_directory(repo_dir, header_file);
    
    fprintf(header_file, "#endif // %s_COMBINED_H\n", repo_name);
    
    fclose(header_file);
    return strdup(header_filename);
}

ConversionResult* convert_git_repository(const char *git_url) {
    ConversionResult *result = malloc(sizeof(ConversionResult));
    memset(result, 0, sizeof(ConversionResult));
    
    result->git_url = strdup(git_url);
    result->repo_name = extract_repo_name(git_url);
    
    if (!result->repo_name) {
        result->error = strdup("Invalid repository URL");
        result->success = 0;
        return result;
    }
    
    create_directory(TEMP_DIR);
    
    char repo_dir[512];
    snprintf(repo_dir, sizeof(repo_dir), "%s/%s", TEMP_DIR, result->repo_name);
    
    cleanup_directory(repo_dir);
    
    printf("Cloning repository: %s\n", git_url);
    if (!clone_repository(git_url, repo_dir)) {
        result->error = strdup("Failed to clone repository");
        result->success = 0;
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
        result->success = 0;
        cleanup_directory(repo_dir);
        return result;
    }
    
    printf("Creating header-only file...\n");
    result->header_filename = create_header_only_file(repo_dir, result->repo_name);
    
    if (!result->header_filename) {
        result->error = strdup("Failed to create header-only file");
        result->success = 0;
        cleanup_directory(repo_dir);
        return result;
    }
    
    cleanup_directory(repo_dir);
    result->success = 1;
    
    printf("Conversion completed successfully!\n");
    return result;
}

json_object* create_json_response(ConversionResult *result) {
    json_object *response = json_object_new_object();
    
    json_object_object_add(response, "success", json_object_new_boolean(result->success));
    
    if (result->success) {
        json_object_object_add(response, "repository", json_object_new_string(result->repo_name));
        json_object_object_add(response, "c_files_count", json_object_new_int(result->c_files_count));
        json_object_object_add(response, "header_files_count", json_object_new_int(result->header_files_count));
        json_object_object_add(response, "filename", json_object_new_string(result->header_filename));
    } else {
        json_object_object_add(response, "error", json_object_new_string(result->error ? result->error : "Unknown error"));
    }
    
    return response;
}

char* read_html_file() {
    return read_file_content("index.html");
}

void send_response(int client_fd, const char *content, const char *content_type, int status_code) {
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

void handle_request(int client_fd, const char *method, const char *url, const char *body) {
    if (strcmp(method, "GET") == 0 && strcmp(url, "/") == 0) {
        char *html_content = read_html_file();
        if (html_content) {
            send_response(client_fd, html_content, "text/html", 200);
            free(html_content);
        } else {
            const char *error_html = "<html><body><h1>Giga-Header</h1><p>Error loading page</p></body></html>";
            send_response(client_fd, error_html, "text/html", 500);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(url, "/convert") == 0) {
        printf("Received conversion request\n");
        
        json_object *request_json = json_tokener_parse(body);
        if (!request_json) {
            const char *error_json = "{\"success\":false,\"error\":\"Invalid JSON\"}";
            send_response(client_fd, error_json, "application/json", 400);
            return;
        }
        
        json_object *git_url_obj;
        if (!json_object_object_get_ex(request_json, "git_url", &git_url_obj)) {
            const char *error_json = "{\"success\":false,\"error\":\"Missing git_url field\"}";
            send_response(client_fd, error_json, "application/json", 400);
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
        
        if (result->git_url) free(result->git_url);
        if (result->repo_name) free(result->repo_name);
        if (result->header_filename) free(result->header_filename);
        if (result->error) free(result->error);
        free(result);
    } else {
        const char *error_json = "{\"success\":false,\"error\":\"Not found\"}";
        send_response(client_fd, error_json, "application/json", 404);
    }
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};
    
    create_directory(TEMP_DIR);
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    printf("Giga-Header Server running on port %d\n", PORT);
    printf("Open http://localhost:%d in your browser\n", PORT);
    printf("Press Ctrl+C to stop the server...\n");
    
    while (1) {
        if ((client_fd = accept(server_fd, (struct sockaddr *)&address, &addrlen)) < 0) {
            perror("accept");
            continue;
        }
        
        read(client_fd, buffer, BUFFER_SIZE);
        
        char method[16], url[256], version[16];
        sscanf(buffer, "%s %s %s", method, url, version);
        
        char *body = strstr(buffer, "\r\n\r\n");
        if (body) body += 4;
        
        handle_request(client_fd, method, url, body ? body : "");
        
        close(client_fd);
        memset(buffer, 0, BUFFER_SIZE);
    }
    
    return 0;
}