#include "net/arp.h"

#include "debug/log.h"
#include "net/ethernet.h"
#include "net/ipv4.h"

#define ARP_HARDWARE_ETHERNET 1U
#define ARP_PROTOCOL_IPV4 0x0800U
#define ARP_OPERATION_REQUEST 1U
#define ARP_OPERATION_REPLY 2U
#define ARP_ADDRESS_CACHE_SIZE 8U

typedef struct {
    uint8_t valid;
    uint8_t ip[4];
    uint8_t mac[6];
} ArpCacheEntry;

static ArpCacheEntry arp_cache[ARP_ADDRESS_CACHE_SIZE];
static uint32_t arp_cache_next;

static uint16_t arp_read_be16(const uint8_t* data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void arp_write_be16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void arp_copy(uint8_t* destination, const uint8_t* source, uint32_t count) {
    for (uint32_t index = 0U; index < count; index++) {
        destination[index] = source[index];
    }
}

static int arp_ip_equal(const uint8_t* left, const uint8_t* right) {
    for (uint32_t index = 0U; index < 4U; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static int arp_mac_equal(const uint8_t* left, const uint8_t* right) {
    for (uint32_t index = 0U; index < 6U; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static void arp_cache_update(const uint8_t ip[4], const uint8_t mac[6]) {
    for (uint32_t index = 0U; index < ARP_ADDRESS_CACHE_SIZE; index++) {
        if (arp_cache[index].valid && arp_ip_equal(arp_cache[index].ip, ip)) {
            arp_copy(arp_cache[index].mac, mac, 6U);
            return;
        }
    }
    arp_cache[arp_cache_next].valid = 1U;
    arp_copy(arp_cache[arp_cache_next].ip, ip, 4U);
    arp_copy(arp_cache[arp_cache_next].mac, mac, 6U);
    arp_cache_next = (arp_cache_next + 1U) % ARP_ADDRESS_CACHE_SIZE;
}

static void arp_receive(network_interface_t* interface, const uint8_t source_mac[6],
    const uint8_t* payload, uint16_t payload_length, void* context) {
    uint16_t operation;
    ipv4_configuration_t configuration;
    (void)context;

    if (payload == 0 || payload_length < 28U
        || arp_read_be16(&payload[0U]) != ARP_HARDWARE_ETHERNET
        || arp_read_be16(&payload[2U]) != ARP_PROTOCOL_IPV4
        || payload[4U] != 6U || payload[5U] != 4U) {
        return;
    }
    operation = arp_read_be16(&payload[6U]);
    if (operation != ARP_OPERATION_REQUEST && operation != ARP_OPERATION_REPLY) {
        return;
    }
    if (!arp_mac_equal(&payload[8U], source_mac) || arp_ip_equal(&payload[14U],
        (const uint8_t[4]){0U, 0U, 0U, 0U})) {
        return;
    }
    arp_cache_update(&payload[14U], &payload[8U]);
    if (operation == ARP_OPERATION_REQUEST
        && ipv4_get_configuration(&configuration) == 0
        && arp_ip_equal(&payload[24U], configuration.address)) {
        uint8_t reply[28];
        arp_write_be16(&reply[0U], ARP_HARDWARE_ETHERNET);
        arp_write_be16(&reply[2U], ARP_PROTOCOL_IPV4);
        reply[4U] = 6U;
        reply[5U] = 4U;
        arp_write_be16(&reply[6U], ARP_OPERATION_REPLY);
        arp_copy(&reply[8U], interface->mac_address, 6U);
        arp_copy(&reply[14U], configuration.address, 4U);
        arp_copy(&reply[18U], &payload[8U], 6U);
        arp_copy(&reply[24U], &payload[14U], 4U);
        (void)ethernet_send(interface, source_mac, ETHERNET_PROTOCOL_ARP,
            reply, sizeof(reply));
    }
    DEBUG_LOG("arp: IPv4 neighbor learned");
}

void arp_init(void) {
    for (uint32_t index = 0U; index < ARP_ADDRESS_CACHE_SIZE; index++) {
        arp_cache[index].valid = 0U;
    }
    arp_cache_next = 0U;
    (void)ethernet_register_protocol(ETHERNET_PROTOCOL_ARP, arp_receive, 0);
}

int arp_request_ipv4(network_interface_t* interface, const uint8_t sender_ip[4],
    const uint8_t target_ip[4]) {
    static const uint8_t broadcast_mac[6] = ETHERNET_BROADCAST_ADDRESS;
    uint8_t packet[28];

    if (interface == 0 || sender_ip == 0 || target_ip == 0) {
        return -1;
    }
    arp_write_be16(&packet[0U], ARP_HARDWARE_ETHERNET);
    arp_write_be16(&packet[2U], ARP_PROTOCOL_IPV4);
    packet[4U] = 6U;
    packet[5U] = 4U;
    arp_write_be16(&packet[6U], ARP_OPERATION_REQUEST);
    arp_copy(&packet[8U], interface->mac_address, 6U);
    arp_copy(&packet[14U], sender_ip, 4U);
    for (uint32_t index = 0U; index < 6U; index++) {
        packet[18U + index] = 0U;
    }
    arp_copy(&packet[24U], target_ip, 4U);
    return ethernet_send(interface, broadcast_mac, ETHERNET_PROTOCOL_ARP,
        packet, sizeof(packet));
}

int arp_lookup_ipv4(const uint8_t ip_address[4], uint8_t out_mac[6]) {
    if (ip_address == 0 || out_mac == 0) {
        return -1;
    }
    for (uint32_t index = 0U; index < ARP_ADDRESS_CACHE_SIZE; index++) {
        if (arp_cache[index].valid && arp_ip_equal(arp_cache[index].ip, ip_address)) {
            arp_copy(out_mac, arp_cache[index].mac, 6U);
            return 0;
        }
    }
    return -1;
}

void arp_learn_ipv4(const uint8_t ip_address[4], const uint8_t mac_address[6]) {
    if (ip_address == 0 || mac_address == 0
        || arp_ip_equal(ip_address, (const uint8_t[4]){0U, 0U, 0U, 0U})) {
        return;
    }
    arp_cache_update(ip_address, mac_address);
}
