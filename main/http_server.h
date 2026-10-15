// main/http_server.h
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_http_server.h"

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE
} http_method_t;

typedef struct {
    const char* path;
    http_method_t method;
    esp_err_t (*handler)(httpd_req_t *req);
} http_endpoint_t;

void init_http_server(void);
void register_http_endpoint(const http_endpoint_t* endpoint);
void unregister_http_endpoint(const char* path);

#endif // HTTP_SERVER_H