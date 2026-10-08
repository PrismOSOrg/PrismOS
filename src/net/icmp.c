#include "net/icmp.h"

#include "debug/log.h"
#include "interrupts/interrupts.h"
#include "net/ipv4.h"
#include "net/network.h"

#define ICMP_ECHO_REPLY 0U
#define ICMP_ECHO_REQUEST 8U
#define ICMP_HEADER_SIZE 8U

static uint16_t next_identifier = 0x5052U;
static uint16_t next_sequence = 1U;
static uint16_t pending_identifier;
static uint16_t pending_sequence;
static uint8_t pending_destination[4];
static uint8_t pending;
static uint8_t reply_received;

static uint16_t read_be16(const uint8_t* data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void write_be16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static int address_equal(const uint8_t* left, const uint8_t* right) {
    for (uint32_t index = 0U; index < 4U; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static void icmp_receive(network_interface_t* interface, const uint8_t source[4],
    const uint8_t destination[4], const uint8_t* packet, uint16_t packet_length,
    void* context) {
    uint8_t response[IPV4_MAX_PAYLOAD];
    uint16_t identifier;
    uint16_t sequence;
    (void)context;

    if (interface == 0 || packet == 0 || packet_length < ICMP_HEADER_SIZE
        || ipv4_checksum(packet, packet_length) != 0U) {
        return;
    }
    identifier = read_be16(&packet[4U]);
    sequence = read_be16(&packet[6U]);
    if (packet[0U] == ICMP_ECHO_REPLY && pending
        && identifier == pending_identifier && sequence == pending_sequence
        && address_equal(source, pending_destination)) {
        reply_received = 1U;
        DEBUG_LOG("icmp: echo reply received");
        return;
    }
    if (packet[0U] != ICMP_ECHO_REQUEST || packet[1U] != 0U
        || packet_length > sizeof(response)) {
        return;
    }
    for (uint32_t index = 0U; index < packet_length; index++) {
        response[index] = packet[index];
    }
    response[0U] = ICMP_ECHO_REPLY;
    response[2U] = 0U;
    response[3U] = 0U;
    write_be16(&response[2U], ipv4_checksum(response, packet_length));
    (void)ipv4_send(interface, source, IPV4_PROTOCOL_ICMP, response, packet_length);
    (void)destination;
}

void icmp_init(void) {
    pending = 0U;
    reply_received = 0U;
    (void)ipv4_register_protocol(IPV4_PROTOCOL_ICMP, icmp_receive, 0);
}

int icmp_ping(const uint8_t destination[4], uint32_t timeout_ms, uint32_t* round_trip_ms) {
    network_interface_t* interface = network_default_interface();
    ipv4_configuration_t configuration;
    uint8_t packet[16];
    uint8_t mac[6];
    uint32_t started;

    if (destination == 0 || interface == 0 || !interface->link_up
        || ipv4_get_configuration(&configuration) != 0
        || (configuration.address[0] == 0U && configuration.address[1] == 0U
            && configuration.address[2] == 0U && configuration.address[3] == 0U)
        || pending) {
        return -1;
    }
    if (ipv4_resolve(destination, timeout_ms, mac) != 0) {
        return -2;
    }
    pending_identifier = next_identifier++;
    pending_sequence = next_sequence++;
    for (uint32_t index = 0U; index < sizeof(packet); index++) {
        packet[index] = 0U;
    }
    packet[0U] = ICMP_ECHO_REQUEST;
    write_be16(&packet[4U], pending_identifier);
    write_be16(&packet[6U], pending_sequence);
    packet[8U] = 'P';
    packet[9U] = 'r';
    packet[10U] = 'i';
    packet[11U] = 's';
    packet[12U] = 'm';
    packet[13U] = 'O';
    packet[14U] = 'S';
    packet[15U] = '!';
    write_be16(&packet[2U], ipv4_checksum(packet, sizeof(packet)));

    pending_destination[0] = destination[0];
    pending_destination[1] = destination[1];
    pending_destination[2] = destination[2];
    pending_destination[3] = destination[3];
    pending = 1U;
    reply_received = 0U;
    started = system_uptime_ms();
    if (ipv4_send(interface, destination, IPV4_PROTOCOL_ICMP, packet, sizeof(packet)) != 0) {
        pending = 0U;
        return -3;
    }
    DEBUG_LOG("icmp: echo request transmitted");
    while (!reply_received && (uint32_t)(system_uptime_ms() - started) < timeout_ms) {
        network_poll();
        __asm__ volatile ("hlt");
    }
    pending = 0U;
    if (!reply_received) {
        return -4;
    }
    if (round_trip_ms != 0) {
        *round_trip_ms = system_uptime_ms() - started;
    }
    return 0;
}
