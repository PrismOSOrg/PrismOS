#include "net/dns.h"

#include "debug/log.h"
#include "interrupts/interrupts.h"
#include "net/ipv4.h"
#include "net/network.h"
#include "net/udp.h"

#define DNS_CLIENT_PORT 53053U
#define DNS_SERVER_PORT 53U
#define DNS_HEADER_SIZE 12U
#define DNS_PACKET_CAPACITY 512U
#define DNS_MAX_NAME_SIZE 254U
#define DNS_CACHE_ENTRIES 4U
#define DNS_TYPE_A 1U
#define DNS_CLASS_IN 1U
#define DNS_FLAG_RESPONSE 0x8000U
#define DNS_RECURSION_DESIRED 0x0100U
#define DNS_MAX_TTL_SECONDS 86400U

typedef struct {
    char hostname[DNS_MAX_NAME_SIZE];
    uint8_t address[4];
    uint32_t expires_ms;
    uint8_t valid;
} DnsCacheEntry;

static uint16_t next_identifier = 0x5052U;
static uint16_t active_identifier;
static uint8_t active_server[4];
static uint8_t active_answer[4];
static uint8_t active_encoded_name[DNS_MAX_NAME_SIZE];
static uint16_t active_encoded_name_length;
static char active_hostname[DNS_MAX_NAME_SIZE];
static int active_result;
static uint8_t resolution_active;
static DnsCacheEntry dns_cache[DNS_CACHE_ENTRIES];
static uint32_t next_cache_entry;

static uint16_t read_be16(const uint8_t* data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
        | ((uint32_t)data[2] << 8) | data[3];
}

static void write_be16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void copy_bytes(uint8_t* destination, const uint8_t* source, uint32_t length) {
    for (uint32_t index = 0U; index < length; index++) {
        destination[index] = source[index];
    }
}

static int address_equal(const uint8_t* left, const uint8_t* right) {
    for (uint32_t index = 0U; index < 4U; index++) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

static char dns_lower(char character) {
    return character >= 'A' && character <= 'Z'
        ? (char)(character - 'A' + 'a') : character;
}

static int dns_hostname_equal(const char* left, const char* right) {
    while (*left != '\0' && *right != '\0') {
        if (dns_lower(*left) != dns_lower(*right)) return 0;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int dns_cache_lookup(const char* hostname, uint8_t address[4]) {
    uint32_t now = system_uptime_ms();
    for (uint32_t index = 0U; index < DNS_CACHE_ENTRIES; index++) {
        if (dns_cache[index].valid && (int32_t)(now - dns_cache[index].expires_ms) < 0
            && dns_hostname_equal(hostname, dns_cache[index].hostname)) {
            copy_bytes(address, dns_cache[index].address, 4U);
            return 0;
        }
    }
    return -1;
}

static void dns_cache_store(const char* hostname, const uint8_t address[4], uint32_t ttl) {
    DnsCacheEntry* entry;
    uint32_t index = next_cache_entry++ % DNS_CACHE_ENTRIES;
    uint32_t length = 0U;
    if (ttl == 0U) return;
    if (ttl > DNS_MAX_TTL_SECONDS) ttl = DNS_MAX_TTL_SECONDS;
    entry = &dns_cache[index];
    while (hostname[length] != '\0' && length + 1U < sizeof(entry->hostname)) {
        entry->hostname[length] = hostname[length];
        length++;
    }
    entry->hostname[length] = '\0';
    copy_bytes(entry->address, address, 4U);
    entry->expires_ms = system_uptime_ms() + ttl * 1000U;
    entry->valid = 1U;
}

static int dns_parse_ipv4_literal(const char* name, uint8_t out_address[4]) {
    const char* cursor = name;
    for (uint32_t octet = 0U; octet < 4U; octet++) {
        uint32_t value = 0U;
        uint32_t digits = 0U;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10U + (uint32_t)(*cursor - '0');
            if (value > 255U) return -1;
            cursor++;
            digits++;
        }
        if (digits == 0U) return -1;
        out_address[octet] = (uint8_t)value;
        if (octet != 3U) {
            if (*cursor != '.') return -1;
            cursor++;
        } else if (*cursor != '\0') {
            return -1;
        }
    }
    return 0;
}

static int dns_encode_name(const char* hostname, uint8_t* output,
    uint16_t capacity, uint16_t* out_length) {
    uint16_t cursor = 0U;
    const char* label = hostname;

    if (hostname == 0 || *hostname == '\0') return -1;
    while (*label != '\0') {
        const char* end = label;
        uint32_t label_length = 0U;
        while (*end != '\0' && *end != '.') {
            char character = *end;
            if (!((character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '-')) {
                return -1;
            }
            end++;
            label_length++;
        }
        if (label_length == 0U || label_length > 63U
            || (uint32_t)cursor + label_length + 1U >= capacity) {
            return -1;
        }
        output[cursor++] = (uint8_t)label_length;
        for (uint32_t index = 0U; index < label_length; index++) {
            output[cursor++] = (uint8_t)label[index];
        }
        if (*end == '.') {
            end++;
            if (*end == '\0') break;
        }
        label = end;
    }
    if (cursor + 1U > capacity || cursor + 1U > DNS_MAX_NAME_SIZE) return -1;
    output[cursor++] = 0U;
    *out_length = cursor;
    return 0;
}

static int dns_skip_name(const uint8_t* packet, uint16_t packet_length, uint16_t* offset) {
    uint16_t cursor = *offset;
    uint32_t labels = 0U;

    while (cursor < packet_length && labels++ < DNS_MAX_NAME_SIZE) {
        uint8_t length = packet[cursor++];
        if (length == 0U) {
            *offset = cursor;
            return 0;
        }
        if ((length & 0xC0U) == 0xC0U) {
            if (cursor >= packet_length) return -1;
            *offset = (uint16_t)(cursor + 1U);
            return 0;
        }
        if ((length & 0xC0U) != 0U || (uint32_t)cursor + length > packet_length) return -1;
        cursor = (uint16_t)(cursor + length);
    }
    return -1;
}

static int dns_send_query(network_interface_t* interface, const ipv4_configuration_t* configuration,
    const uint8_t* encoded_name, uint16_t encoded_name_length) {
    uint8_t query[DNS_PACKET_CAPACITY];
    uint16_t length = DNS_HEADER_SIZE;

    for (uint32_t index = 0U; index < sizeof(query); index++) query[index] = 0U;
    write_be16(&query[0U], active_identifier);
    write_be16(&query[2U], DNS_RECURSION_DESIRED);
    write_be16(&query[4U], 1U);
    if ((uint32_t)length + encoded_name_length + 4U > sizeof(query)) return -1;
    copy_bytes(&query[length], encoded_name, encoded_name_length);
    length = (uint16_t)(length + encoded_name_length);
    write_be16(&query[length], DNS_TYPE_A);
    write_be16(&query[length + 2U], DNS_CLASS_IN);
    length = (uint16_t)(length + 4U);
    return udp_send(interface, configuration->address, configuration->dns,
        DNS_CLIENT_PORT, DNS_SERVER_PORT, query, length);
}

static void dns_receive(network_interface_t* interface, const uint8_t source[4],
    const uint8_t destination[4], uint16_t source_port, uint16_t destination_port,
    const uint8_t* packet, uint16_t packet_length, void* context) {
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t offset = DNS_HEADER_SIZE;
    uint32_t answer_ttl = 0U;
    (void)interface;
    (void)destination;
    (void)context;

    if (!resolution_active || packet == 0 || packet_length < DNS_HEADER_SIZE
        || source_port != DNS_SERVER_PORT || destination_port != DNS_CLIENT_PORT
        || !address_equal(source, active_server)
        || read_be16(&packet[0U]) != active_identifier) {
        return;
    }
    flags = read_be16(&packet[2U]);
    if ((flags & DNS_FLAG_RESPONSE) == 0U) return;
    if ((flags & 0x000FU) != 0U) {
        active_result = -3;
        return;
    }
    question_count = read_be16(&packet[4U]);
    answer_count = read_be16(&packet[6U]);
    if (question_count != 1U || active_encoded_name_length == 0U
        || (uint32_t)offset + active_encoded_name_length + 4U > packet_length) {
        active_result = -3;
        return;
    }
    for (uint32_t index = 0U; index < active_encoded_name_length; index++) {
        if (packet[offset + index] != active_encoded_name[index]) {
            active_result = -3;
            return;
        }
    }
    offset = (uint16_t)(offset + active_encoded_name_length);
    if (read_be16(&packet[offset]) != DNS_TYPE_A
        || read_be16(&packet[offset + 2U]) != DNS_CLASS_IN) {
        active_result = -3;
        return;
    }
    offset = (uint16_t)(offset + 4U);
    for (uint32_t answer = 0U; answer < answer_count; answer++) {
        uint16_t type;
        uint16_t record_class;
        uint16_t data_length;
        if (dns_skip_name(packet, packet_length, &offset) != 0
            || (uint32_t)offset + 10U > packet_length) {
            active_result = -3;
            return;
        }
        type = read_be16(&packet[offset]);
        record_class = read_be16(&packet[offset + 2U]);
        answer_ttl = read_be32(&packet[offset + 4U]);
        data_length = read_be16(&packet[offset + 8U]);
        offset = (uint16_t)(offset + 10U);
        if ((uint32_t)offset + data_length > packet_length) {
            active_result = -3;
            return;
        }
        if (type == DNS_TYPE_A && record_class == DNS_CLASS_IN && data_length == 4U) {
            copy_bytes(active_answer, &packet[offset], 4U);
            active_result = 1;
            dns_cache_store(active_hostname, active_answer, answer_ttl);
            DEBUG_LOG("dns: IPv4 address resolved");
            return;
        }
        offset = (uint16_t)(offset + data_length);
    }
    active_result = -3;
}

void dns_init(void) {
    active_result = 0;
    resolution_active = 0U;
    next_cache_entry = 0U;
    active_hostname[0] = '\0';
    for (uint32_t index = 0U; index < DNS_CACHE_ENTRIES; index++) dns_cache[index].valid = 0U;
    (void)udp_register_port(DNS_CLIENT_PORT, dns_receive, 0);
}

int dns_resolve_ipv4(const char* hostname, uint32_t timeout_ms, uint8_t out_address[4]) {
    network_interface_t* interface = network_default_interface();
    ipv4_configuration_t configuration;
    uint8_t encoded_name[DNS_MAX_NAME_SIZE];
    uint16_t encoded_name_length;
    uint32_t started;
    uint32_t half_timeout;
    uint8_t dns_server_mac[6];

    if (hostname == 0 || out_address == 0) return -1;
    if (dns_parse_ipv4_literal(hostname, out_address) == 0) return 0;
    if (dns_cache_lookup(hostname, out_address) == 0) return 0;
    if (interface == 0 || !interface->link_up || ipv4_get_configuration(&configuration) != 0
        || (configuration.dns[0] == 0U && configuration.dns[1] == 0U
            && configuration.dns[2] == 0U && configuration.dns[3] == 0U)
        || resolution_active || timeout_ms == 0U
        || dns_encode_name(hostname, encoded_name, sizeof(encoded_name), &encoded_name_length) != 0) {
        return -1;
    }
    if (ipv4_resolve(configuration.dns, timeout_ms, dns_server_mac) != 0) return -2;

    active_identifier = next_identifier++;
    active_server[0] = configuration.dns[0];
    active_server[1] = configuration.dns[1];
    active_server[2] = configuration.dns[2];
    active_server[3] = configuration.dns[3];
    active_result = 0;
    active_encoded_name_length = encoded_name_length;
    copy_bytes(active_encoded_name, encoded_name, encoded_name_length);
    uint32_t hostname_length = 0U;
    while (hostname[hostname_length] != '\0' && hostname_length + 1U < sizeof(active_hostname)) {
        active_hostname[hostname_length] = hostname[hostname_length];
        hostname_length++;
    }
    active_hostname[hostname_length] = '\0';
    resolution_active = 1U;
    if (dns_send_query(interface, &configuration, encoded_name, encoded_name_length) != 0) {
        resolution_active = 0U;
        return -1;
    }
    DEBUG_LOG("dns: A query transmitted");
    started = system_uptime_ms();
    half_timeout = timeout_ms / 2U;
    if (half_timeout == 0U) half_timeout = timeout_ms;
    uint8_t retried = 0U;

    while (active_result == 0 && (uint32_t)(system_uptime_ms() - started) < timeout_ms) {
        uint32_t elapsed = system_uptime_ms() - started;
        if (!retried && elapsed >= half_timeout) {
            (void)dns_send_query(interface, &configuration, encoded_name, encoded_name_length);
            retried = 1U;
        }
        network_poll();
        __asm__ volatile ("hlt");
    }
    resolution_active = 0U;
    if (active_result == 1) {
        copy_bytes(out_address, active_answer, 4U);
        return 0;
    }
    return active_result < 0 ? active_result : -2;
}
