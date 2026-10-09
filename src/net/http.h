#ifndef PRISMOS_NET_HTTP_H
#define PRISMOS_NET_HTTP_H

#include <stdint.h>

#define HTTP_MAX_BODY_SIZE 8192U
#define HTTP_CONTENT_TYPE_SIZE 64U

typedef struct {
    uint16_t status_code;
    uint32_t body_length;
    uint8_t* body;
    uint32_t body_capacity;
    char content_type[HTTP_CONTENT_TYPE_SIZE];
} http_response_t;

/* HTTP/1.0 GET over TCP. HTTPS/TLS is intentionally not included. */
int http_get(const char* hostname, const char* path, uint16_t port,
    uint32_t timeout_ms, http_response_t* response);

#endif
