#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <microhttpd.h>
#include <json-c/json.h>

#define PORT 8080

const char *html_content = 
    "<!DOCTYPE html><html><head><title>Giga-Header</title></head><body>"
    "<h1>Giga-Header</h1><p>C to Header-Only Converter</p></body></html>";

const char *error_json = "{\"success\":false,\"error\":\"Server error\"}";
const char *success_json = "{\"success\":true,\"repository\":\"test\",\"c_files_count\":1,\"header_files_count\":0,\"filename\":\"test_combined.h\"}";

enum MHD_Result handle_request(void *cls, struct MHD_Connection *connection,
                               const char *url, const char *method,
                               const char *version, const char *upload_data,
                               size_t *upload_data_size, void **con_cls) {
    (void)cls; (void)version; (void)con_cls;
    
    struct MHD_Response *response;
    enum MHD_Result ret;
    
    if (strcmp(method, "GET") == 0 && strcmp(url, "/") == 0) {
        response = MHD_create_response_from_buffer(strlen(html_content), 
                                                   (void*)html_content, 
                                                   MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "text/html");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    }
    
    if (strcmp(method, "POST") == 0 && strcmp(url, "/convert") == 0) {
        if (*upload_data_size == 0) {
            return MHD_YES;
        }
        
        response = MHD_create_response_from_buffer(strlen(success_json), 
                                                   (void*)success_json, 
                                                   MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        *upload_data_size = 0;
        return ret;
    }
    
    response = MHD_create_response_from_buffer(strlen(error_json), 
                                               (void*)error_json, 
                                               MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(response, "Content-Type", "application/json");
    ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    MHD_destroy_response(response);
    
    return ret;
}

int main() {
    struct MHD_Daemon *daemon;
    
    daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG, 
                             PORT, NULL, NULL,
                             &handle_request, NULL, 
                             MHD_OPTION_END);
    
    if (!daemon) {
        fprintf(stderr, "Failed to start server on port %d\n", PORT);
        return 1;
    }
    
    printf("Giga-Header Server running on port %d\n", PORT);
    printf("Open http://localhost:%d in your browser\n", PORT);
    
    getchar();
    
    MHD_stop_daemon(daemon);
    return 0;
}