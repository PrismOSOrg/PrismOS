#include "net/ipv4.h"

#include "debug/log.h"
#include "interrupts/interrupts.h"
#include "net/arp.h"
#include "net/ethernet.h"

#define IPV4_MAX_PROTOCOL_HANDLERS 8U
#define IPV4_FLAG_DONT_FRAGMENT 0x4000U
#define IPV4_FRAGMENT_MASK 0x3FFFU

static ipv4_configuration_t active_configuration;
static network_interface_t* active_interface;
static ipv4_protocol_handler_t protocol_handlers[IPV4_MAX_PROTOCOL_HANDLERS];
static void* protocol_contexts[IPV4_MAX_PROTOCOL_HANDLERS];
static uint8_t protocol_numbers[IPV4_MAX_PROTOCOL_HANDLERS];
static uint32_t protocol_handler_count;

static uint16_t read_be16(const uint8_t* data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
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
    for (uint32_t index = 0U; index < IPV4_ADDRESS_SIZE; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static int address_is_zero(const uint8_t* address) {
    return address[0] == 0U && address[1] == 0U && address[2] == 0U && address[3] == 0U;
}

uint16_t ipv4_checksum(const uint8_t* data, uint16_t length) {
    uint32_t sum = 0U;
    uint16_t index = 0U;

    if (data == 0) {
        return 0U;
    }
    while ((uint32_t)index + 1U < length) {
        sum += ((uint32_t)data[index] << 8) | data[index + 1U];
        index = (uint16_t)(index + 2U);
    }
    if (index < length) {
        sum += (uint32_t)data[index] << 8;
    }
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

uint16_t ipv4_transport_checksum(const uint8_t source[4], const uint8_t destination[4],
    uint8_t protocol, const uint8_t* segment, uint16_t length) {
    uint32_t sum = 0U;
    uint16_t index = 0U;

    if (source == 0 || destination == 0 || (length != 0U && segment == 0)) {
        return 0U;
    }
    for (uint32_t i = 0U; i < 4U; i += 2U) {
        sum += ((uint32_t)source[i] << 8) | source[i + 1U];
        sum += ((uint32_t)destination[i] << 8) | destination[i + 1U];
    }
    sum += protocol;
    sum += length;
    while ((uint32_t)index + 1U < length) {
        sum += ((uint32_t)segment[index] << 8) | segment[index + 1U];
        index = (uint16_t)(index + 2U);
    }
    if (index < length) {
        sum += (uint32_t)segment[index] << 8;
    }
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static int ipv4_is_broadcast(const uint8_t address[4]) {
    if (address[0] == 255U && address[1] == 255U
        && address[2] == 255U && address[3] == 255U) {
        return 1;
    }
    if (!address_is_zero(active_configuration.netmask)) {
        int all_host_bits_set = 1;
        for (uint32_t index = 0U; index < 4U; index++) {
            if ((address[index] & active_configuration.netmask[index])
                    != (active_configuration.address[index] & active_configuration.netmask[index])
                || (uint8_t)(address[index] | active_configuration.netmask[index]) != 255U) {
                all_host_bits_set = 0;
            }
        }
        return all_host_bits_set;
    }
    return 0;
}

static void ipv4_receive(network_interface_t* interface, const uint8_t source_mac[6],
    const uint8_t* packet, uint16_t packet_length, void* context) {
    uint8_t header_length;
    uint16_t total_length;
    uint16_t fragment;
    uint8_t protocol;
    const uint8_t* source;
    const uint8_t* destination;
    (void)source_mac;
    (void)context;

    if (packet == 0 || packet_length < IPV4_HEADER_SIZE || (packet[0] >> 4) != 4U) {
        return;
    }
    header_length = (uint8_t)((packet[0] & 0x0FU) * 4U);
    total_length = read_be16(&packet[2U]);
    if (header_length < IPV4_HEADER_SIZE || header_length > packet_length
        || total_length < header_length || total_length > packet_length
        || ipv4_checksum(packet, header_length) != 0U) {
        return;
    }
    fragment = read_be16(&packet[6U]);
    if ((fragment & IPV4_FRAGMENT_MASK) != 0U) {
        return;
    }
    source = &packet[12U];
    destination = &packet[16U];
    if (!ipv4_is_broadcast(destination)
        && (!ipv4_is_configured() || !address_equal(destination, active_configuration.address))) {
        return;
    }
    if (!address_is_zero(source)) {
        arp_learn_ipv4(source, source_mac);
    }
    protocol = packet[9U];
    for (uint32_t index = 0U; index < protocol_handler_count; index++) {
        if (protocol_numbers[index] == protocol) {
            protocol_handlers[index](interface, source, destination,
                &packet[header_length], (uint16_t)(total_length - header_length),
                protocol_contexts[index]);
            return;
        }
    }
}

void ipv4_init(void) {
    for (uint32_t index = 0U; index < sizeof(active_configuration); index++) {
        ((uint8_t*)&active_configuration)[index] = 0U;
    }
    active_interface = 0;
    protocol_handler_count = 0U;
    if (ethernet_register_protocol(ETHERNET_PROTOCOL_IPV4, ipv4_receive, 0) != 0) {
        ERROR_LOG("ipv4: Ethernet handler registration failed");
    }
}

int ipv4_register_protocol(uint8_t protocol, ipv4_protocol_handler_t handler, void* context) {
    if (handler == 0 || protocol_handler_count >= IPV4_MAX_PROTOCOL_HANDLERS) {
        return -1;
    }
    for (uint32_t index = 0U; index < protocol_handler_count; index++) {
        if (protocol_numbers[index] == protocol) {
            return -1;
        }
    }
    protocol_numbers[protocol_handler_count] = protocol;
    protocol_handlers[protocol_handler_count] = handler;
    protocol_contexts[protocol_handler_count] = context;
    protocol_handler_count++;
    return 0;
}

int ipv4_configure(network_interface_t* interface, const ipv4_configuration_t* configuration) {
    if (interface == 0 || configuration == 0 || interface != network_default_interface()) {
        return -1;
    }
    copy_bytes((uint8_t*)&active_configuration, (const uint8_t*)configuration,
        sizeof(active_configuration));
    active_interface = interface;
    DEBUG_LOG("ipv4: interface configuration updated");
    return 0;
}

int ipv4_get_configuration(ipv4_configuration_t* configuration) {
    if (configuration == 0 || active_interface == 0) {
        return -1;
    }
    copy_bytes((uint8_t*)configuration, (const uint8_t*)&active_configuration,
        sizeof(active_configuration));
    return 0;
}

int ipv4_is_configured(void) {
    return active_interface != 0 && !address_is_zero(active_configuration.address);
}

int ipv4_resolve(const uint8_t address[4], uint32_t timeout_ms, uint8_t out_mac[6]) {
    network_interface_t* interface = network_default_interface();
    uint8_t route_address[4];
    uint32_t started;

    if (address == 0 || out_mac == 0 || interface == 0 || !ipv4_is_configured()) {
        return -1;
    }
    copy_bytes(route_address, address, 4U);
    if (!address_is_zero(active_configuration.gateway)) {
        int same_subnet = 1;
        for (uint32_t index = 0U; index < 4U; index++) {
            if ((address[index] & active_configuration.netmask[index])
                != (active_configuration.address[index] & active_configuration.netmask[index])) {
                same_subnet = 0;
                break;
            }
        }
        if (!same_subnet) {
            copy_bytes(route_address, active_configuration.gateway, 4U);
        }
    }
    if (arp_lookup_ipv4(route_address, out_mac) == 0) {
        return 0;
    }
    if (arp_request_ipv4(interface, active_configuration.address, route_address) != 0) {
        return -1;
    }
    started = system_uptime_ms();
    while ((uint32_t)(system_uptime_ms() - started) < timeout_ms) {
        network_poll();
        if (arp_lookup_ipv4(route_address, out_mac) == 0) {
            return 0;
        }
        __asm__ volatile ("hlt");
    }
    return -1;
}

int ipv4_send_from(network_interface_t* interface, const uint8_t source[4],
    const uint8_t destination[4], uint8_t protocol, const uint8_t* payload,
    uint16_t payload_length) {
    static const uint8_t broadcast_mac[6] = ETHERNET_BROADCAST_ADDRESS;
    uint8_t packet[NETWORK_MAX_FRAME_SIZE];
    uint8_t destination_mac[6];
    uint32_t total_length = IPV4_HEADER_SIZE + (uint32_t)payload_length;
    uint16_t header_checksum;
    int broadcast;

    if (interface == 0 || source == 0 || destination == 0
        || (payload_length != 0U && payload == 0)
        || total_length > interface->mtu || total_length > sizeof(packet)) {
        return -1;
    }
    broadcast = ipv4_is_broadcast(destination);
    if (broadcast) {
        copy_bytes(destination_mac, broadcast_mac, 6U);
    } else {
        uint8_t route_address[4];
        copy_bytes(route_address, destination, 4U);
        if (interface == active_interface && ipv4_is_configured()
            && !address_is_zero(active_configuration.gateway)) {
            int same_subnet = 1;
            for (uint32_t index = 0U; index < 4U; index++) {
                if ((destination[index] & active_configuration.netmask[index])
                    != (active_configuration.address[index] & active_configuration.netmask[index])) {
                    same_subnet = 0;
                    break;
                }
            }
            if (!same_subnet) {
                copy_bytes(route_address, active_configuration.gateway, 4U);
            }
        }
        if (arp_lookup_ipv4(route_address, destination_mac) != 0) {
            if (interface == active_interface && ipv4_is_configured()) {
                (void)arp_request_ipv4(interface, active_configuration.address, route_address);
            }
            return -1;
        }
    }

    packet[0] = 0x45U;
    packet[1] = 0U;
    write_be16(&packet[2U], (uint16_t)total_length);
    write_be16(&packet[4U], (uint16_t)system_uptime_ms());
    write_be16(&packet[6U], IPV4_FLAG_DONT_FRAGMENT);
    packet[8U] = 64U;
    packet[9U] = protocol;
    packet[10U] = 0U;
    packet[11U] = 0U;
    copy_bytes(&packet[12U], source, 4U);
    copy_bytes(&packet[16U], destination, 4U);
    header_checksum = ipv4_checksum(packet, IPV4_HEADER_SIZE);
    write_be16(&packet[10U], header_checksum);
    copy_bytes(&packet[IPV4_HEADER_SIZE], payload, payload_length);
    return ethernet_send(interface, destination_mac, ETHERNET_PROTOCOL_IPV4,
        packet, (uint16_t)total_length);
}

int ipv4_send(network_interface_t* interface, const uint8_t destination[4],
    uint8_t protocol, const uint8_t* payload, uint16_t payload_length) {
    if (interface != active_interface || !ipv4_is_configured()) {
        return -1;
    }
    return ipv4_send_from(interface, active_configuration.address, destination,
        protocol, payload, payload_length);
}
