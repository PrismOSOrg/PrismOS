#include "net/http.h"

#include "debug/log.h"
#include "interrupts/interrupts.h"
#include "net/dns.h"
#include "net/network.h"
#include "net/tcp.h"

#define HTTP_REQUEST_CAPACITY 512U
#define HTTP_RAW_RESPONSE_CAPACITY (HTTP_MAX_BODY_SIZE + 2048U)
#define HTTP_READ_CHUNK_SIZE 512U

static uint8_t raw_response[HTTP_RAW_RESPONSE_CAPACITY];

static uint32_t string_length(const char* text) {
    uint32_t length = 0U;
    while (text[length] != '\0') length++;
    return length;
}

static int http_append(char* destination, uint32_t capacity, uint32_t* length,
    const char* source, uint32_t source_length) {
    if (*length + source_length >= capacity) return -1;
    for (uint32_t index = 0U; index < source_length; index++) {
        destination[(*length)++] = source[index];
    }
    destination[*length] = '\0';
    return 0;
}

static int http_valid_host(const char* hostname) {
    uint32_t length = string_length(hostname);
    if (length == 0U || length > 253U) return 0;
    for (uint32_t index = 0U; index < length; index++) {
        char character = hostname[index];
        if (!((character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '-')) {
            return 0;
        }
    }
    return 1;
}

static int http_valid_path(const char* path) {
    if (path == 0 || path[0] != '/') return 0;
    for (uint32_t index = 0U; path[index] != '\0'; index++) {
        if ((uint8_t)path[index] < 33U || (uint8_t)path[index] > 126U
            || path[index] == '\r' || path[index] == '\n') {
            return 0;
        }
    }
    return 1;
}

static char http_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int http_equal_case(const uint8_t* data, uint32_t length, const char* text) {
    uint32_t text_length = string_length(text);
    if (length != text_length) return 0;
    for (uint32_t index = 0U; index < length; index++) {
        if (http_lower((char)data[index]) != http_lower(text[index])) return 0;
    }
    return 1;
}

static int http_contains_case(const uint8_t* data, uint32_t length, const char* text) {
    uint32_t text_length = string_length(text);
    if (text_length > length) return 0;
    for (uint32_t start = 0U; start + text_length <= length; start++) {
        uint32_t index = 0U;
        while (index < text_length
            && http_lower((char)data[start + index]) == http_lower(text[index])) {
            index++;
        }
        if (index == text_length) return 1;
    }
    return 0;
}

static int http_find_header_end(const uint8_t* data, uint32_t length, uint32_t* offset) {
    for (uint32_t index = 0U; index + 3U < length; index++) {
        if (data[index] == '\r' && data[index + 1U] == '\n'
            && data[index + 2U] == '\r' && data[index + 3U] == '\n') {
            *offset = index + 4U;
            return 0;
        }
    }
    return -1;
}

static int http_parse_decimal(const uint8_t* data, uint32_t length, uint32_t* value) {
    uint32_t result = 0U;
    uint32_t index = 0U;
    while (index < length && (data[index] == ' ' || data[index] == '\t')) index++;
    if (index == length || data[index] < '0' || data[index] > '9') return -1;
    for (; index < length && data[index] >= '0' && data[index] <= '9'; index++) {
        uint32_t digit = (uint32_t)(data[index] - '0');
        if (result > (0xFFFFFFFFU - digit) / 10U) return -1;
        result = result * 10U + digit;
    }
    *value = result;
    return 0;
}

static int http_parse_status(const uint8_t* data, uint32_t length, uint16_t* status) {
    uint32_t cursor = 0U;
    if (length < 12U || data[0U] != 'H' || data[1U] != 'T' || data[2U] != 'T'
        || data[3U] != 'P' || data[4U] != '/') return -1;
    while (cursor < length && data[cursor] != ' ') cursor++;
    while (cursor < length && data[cursor] == ' ') cursor++;
    if (cursor + 3U > length || data[cursor] < '0' || data[cursor] > '9'
        || data[cursor + 1U] < '0' || data[cursor + 1U] > '9'
        || data[cursor + 2U] < '0' || data[cursor + 2U] > '9') return -1;
    *status = (uint16_t)((data[cursor] - '0') * 100U
        + (data[cursor + 1U] - '0') * 10U + data[cursor + 2U] - '0');
    return 0;
}

static void http_copy_content_type(http_response_t* response, const uint8_t* data,
    uint32_t length) {
    for (uint32_t line = 0U; line + 1U < length;) {
        uint32_t line_end = line;
        while (line_end + 1U < length
            && !(data[line_end] == '\r' && data[line_end + 1U] == '\n')) line_end++;
        if (line_end == line) break;
        uint32_t colon = line;
        while (colon < line_end && data[colon] != ':') colon++;
        if (colon < line_end && http_equal_case(&data[line], colon - line, "Content-Type")) {
            uint32_t value = colon + 1U;
            while (value < line_end && (data[value] == ' ' || data[value] == '\t')) value++;
            uint32_t copy_length = line_end - value;
            if (copy_length >= sizeof(response->content_type)) {
                copy_length = sizeof(response->content_type) - 1U;
            }
            for (uint32_t index = 0U; index < copy_length; index++) {
                response->content_type[index] = (char)data[value + index];
            }
            response->content_type[copy_length] = '\0';
            return;
        }
        line = line_end + 2U;
    }
}

static int http_decode_chunks(const uint8_t* input, uint32_t input_length,
    uint8_t* output, uint32_t output_capacity, uint32_t* output_length) {
    uint32_t cursor = 0U;
    uint32_t written = 0U;
    while (cursor < input_length) {
        uint32_t chunk_size = 0U;
        uint32_t digits = 0U;
        while (cursor < input_length && input[cursor] != '\r' && input[cursor] != ';') {
            uint8_t character = input[cursor++];
            uint32_t value;
            if (character >= '0' && character <= '9') value = character - '0';
            else if (character >= 'a' && character <= 'f') value = character - 'a' + 10U;
            else if (character >= 'A' && character <= 'F') value = character - 'A' + 10U;
            else return -1;
            if (chunk_size > (0xFFFFFFFFU - value) / 16U) return -1;
            chunk_size = chunk_size * 16U + value;
            digits++;
        }
        if (digits == 0U) return -1;
        while (cursor < input_length && input[cursor] != '\r') cursor++;
        if (cursor + 1U >= input_length || input[cursor] != '\r' || input[cursor + 1U] != '\n') return -1;
        cursor += 2U;
        if (chunk_size == 0U) {
            *output_length = written;
            return 0;
        }
        if (chunk_size > input_length - cursor || cursor + chunk_size + 2U > input_length
            || chunk_size > output_capacity - written) return -1;
        for (uint32_t index = 0U; index < chunk_size; index++) output[written++] = input[cursor++];
        if (input[cursor] != '\r' || input[cursor + 1U] != '\n') return -1;
        cursor += 2U;
    }
    return -1;
}

static int http_parse_response(const uint8_t* raw, uint32_t raw_length,
    http_response_t* response) {
    uint32_t body_offset;
    uint32_t header_end;
    uint32_t content_length = 0U;
    uint8_t have_content_length = 0U;
    uint8_t chunked = 0U;
    uint32_t body_available;

    if (http_find_header_end(raw, raw_length, &body_offset) != 0) {
        DEBUG_LOG("http: response has no complete header terminator");
        return -1;
    }
    header_end = body_offset - 4U;
    uint32_t status_line_end = 0U;
    while (status_line_end + 1U < header_end
        && !(raw[status_line_end] == '\r' && raw[status_line_end + 1U] == '\n')) status_line_end++;
    if (http_parse_status(raw, status_line_end, &response->status_code) != 0) {
        DEBUG_LOG("http: invalid status line");
        return -1;
    }

    for (uint32_t line = status_line_end + 2U; line + 1U < header_end;) {
        uint32_t line_end = line;
        uint32_t colon = line;
        while (line_end + 1U < header_end
            && !(raw[line_end] == '\r' && raw[line_end + 1U] == '\n')) line_end++;
        if (line_end == line) break;
        while (colon < line_end && raw[colon] != ':') colon++;
        if (colon < line_end) {
            uint32_t value = colon + 1U;
            while (value < line_end && (raw[value] == ' ' || raw[value] == '\t')) value++;
            if (http_equal_case(&raw[line], colon - line, "Content-Length")) {
                if (http_parse_decimal(&raw[value], line_end - value, &content_length) != 0) {
                    DEBUG_LOG("http: invalid Content-Length header");
                    return -1;
                }
                have_content_length = 1U;
            } else if (http_equal_case(&raw[line], colon - line, "Transfer-Encoding")) {
                chunked = (uint8_t)http_contains_case(&raw[value], line_end - value, "chunked");
            }
        }
        line = line_end + 2U;
    }
    http_copy_content_type(response, raw, header_end);
    body_available = raw_length - body_offset;

    if (chunked) {
        if (http_decode_chunks(&raw[body_offset], body_available, response->body,
            response->body_capacity, &response->body_length) != 0) {
            DEBUG_LOG("http: invalid chunked response body");
            return -1;
        }
    } else {
        uint32_t copy_length = body_available;
        if (have_content_length) {
            if (content_length > body_available) return -2;
            copy_length = content_length;
        }
        if (copy_length > response->body_capacity) return -3;
        for (uint32_t index = 0U; index < copy_length; index++) response->body[index] = raw[body_offset + index];
        response->body_length = copy_length;
    }
    return 0;
}

int http_get(const char* hostname, const char* path, uint16_t port,
    uint32_t timeout_ms, http_response_t* response) {
    static const char get_prefix[] = "GET ";
    static const char http_version[] = " HTTP/1.0\r\nHost: ";
    static const char port_separator[] = ":";
    static const char user_agent[] = "\r\nUser-Agent: PrismOS/0.1\r\nConnection: close\r\n\r\n";
    network_interface_t* interface = network_default_interface();
    uint8_t destination[4];
    char request[HTTP_REQUEST_CAPACITY];
    char port_text[6];
    uint32_t request_length = 0U;
    uint32_t started;
    uint32_t raw_length = 0U;
    uint32_t elapsed;
    uint32_t port_length = 0U;
    int result;

    if (hostname == 0 || response == 0 || response->body == 0
        || response->body_capacity == 0U || port == 0U || timeout_ms == 0U
        || !http_valid_host(hostname) || !http_valid_path(path)
        || interface == 0 || !interface->link_up) return -1;
    response->status_code = 0U;
    response->body_length = 0U;
    response->content_type[0] = '\0';
    if (port != 80U) {
        uint32_t value = port;
        do {
            port_text[port_length++] = (char)('0' + value % 10U);
            value /= 10U;
        } while (value != 0U);
        for (uint32_t left = 0U, right = port_length - 1U; left < right; left++, right--) {
            char temporary = port_text[left];
            port_text[left] = port_text[right];
            port_text[right] = temporary;
        }
        port_text[port_length] = '\0';
    }

    if (dns_resolve_ipv4(hostname, timeout_ms, destination) != 0) {
        DEBUG_LOG("http: DNS lookup failed");
        return -2;
    }
    if (tcp_connect(destination, port, timeout_ms) != 0) {
        DEBUG_LOG("http: TCP connection failed");
        return -3;
    }
    DEBUG_LOG("http: TCP connection ready");

    request[0] = '\0';
    if (http_append(request, sizeof(request), &request_length, get_prefix, sizeof(get_prefix) - 1U) != 0
        || http_append(request, sizeof(request), &request_length, path, string_length(path)) != 0
        || http_append(request, sizeof(request), &request_length, http_version, sizeof(http_version) - 1U) != 0
        || http_append(request, sizeof(request), &request_length, hostname, string_length(hostname)) != 0
        || (port != 80U && (http_append(request, sizeof(request), &request_length,
            port_separator, sizeof(port_separator) - 1U) != 0
            || http_append(request, sizeof(request), &request_length, port_text, port_length) != 0))
        || http_append(request, sizeof(request), &request_length, user_agent, sizeof(user_agent) - 1U) != 0) {
        (void)tcp_close(1000U);
        return -1;
    }
    if (tcp_send((const uint8_t*)request, (uint16_t)request_length, timeout_ms) != 0) {
        (void)tcp_close(1000U);
        DEBUG_LOG("http: request transmission failed");
        return -4;
    }
    DEBUG_LOG("http: request sent and acknowledged");

    started = system_uptime_ms();
    while (raw_length < sizeof(raw_response)) {
        uint8_t chunk[HTTP_READ_CHUNK_SIZE];
        uint16_t chunk_length = 0U;
        elapsed = system_uptime_ms() - started;
        if (elapsed >= timeout_ms) {
            (void)tcp_close(1000U);
            return -5;
        }
        result = tcp_read_wait(chunk, sizeof(chunk), &chunk_length, timeout_ms - elapsed);
        if (result == -2) break;
        if (result != 0) {
            DEBUG_LOG(raw_length == 0U
                ? "http: timed out waiting for the first response bytes"
                : "http: timed out waiting for the rest of the response");
            (void)tcp_close(1000U);
            return -5;
        }
        DEBUG_LOG("http: received a TCP response payload");
        for (uint32_t index = 0U; index < chunk_length; index++) {
            raw_response[raw_length++] = chunk[index];
        }
    }
    (void)tcp_close(1000U);
    DEBUG_LOG("http: response stream complete");
    if (raw_length == 0U) DEBUG_LOG("http: peer closed without sending response bytes");
    result = http_parse_response(raw_response, raw_length, response);
    if (result == -2) {
        DEBUG_LOG("http: incomplete Content-Length body");
        return -5;
    }
    if (result == -3) {
        DEBUG_LOG("http: response body exceeds caller buffer");
        return -6;
    }
    if (result != 0) {
        DEBUG_LOG("http: invalid or unsupported response");
        return -7;
    }
    DEBUG_LOG("http: response received");
    return 0;
}
