#include "net/udp.h"

#include "net/ipv4.h"

#define UDP_PROTOCOL_NUMBER 17U
#define UDP_HEADER_SIZE 8U
#define UDP_MAX_HANDLERS 8U

typedef struct {
    uint16_t port;
    udp_port_handler_t handler;
    void* context;
} UdpHandler;

static UdpHandler handlers[UDP_MAX_HANDLERS];
static uint32_t handler_count;

static uint16_t read_be16(const uint8_t* data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void write_be16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void udp_receive(network_interface_t* interface, const uint8_t source[4],
    const uint8_t destination[4], const uint8_t* packet, uint16_t packet_length,
    void* context) {
    uint16_t length;
    uint16_t checksum;
    uint16_t destination_port;
    (void)context;

    if (packet == 0 || packet_length < UDP_HEADER_SIZE) {
        return;
    }
    length = read_be16(&packet[4U]);
    checksum = read_be16(&packet[6U]);
    if (length < UDP_HEADER_SIZE || length > packet_length) {
        return;
    }
    if (checksum != 0U && ipv4_transport_checksum(source, destination,
        UDP_PROTOCOL_NUMBER, packet, length) != 0U) {
        return;
    }
    destination_port = read_be16(&packet[2U]);
    for (uint32_t index = 0U; index < handler_count; index++) {
        if (handlers[index].port == destination_port) {
            handlers[index].handler(interface, source, destination,
                read_be16(&packet[0U]), destination_port, &packet[UDP_HEADER_SIZE],
                (uint16_t)(length - UDP_HEADER_SIZE), handlers[index].context);
            return;
        }
    }
}

void udp_init(void) {
    handler_count = 0U;
    (void)ipv4_register_protocol(UDP_PROTOCOL_NUMBER, udp_receive, 0);
}

int udp_register_port(uint16_t port, udp_port_handler_t handler, void* context) {
    if (port == 0U || handler == 0 || handler_count >= UDP_MAX_HANDLERS) {
        return -1;
    }
    for (uint32_t index = 0U; index < handler_count; index++) {
        if (handlers[index].port == port) {
            return -1;
        }
    }
    handlers[handler_count].port = port;
    handlers[handler_count].handler = handler;
    handlers[handler_count].context = context;
    handler_count++;
    return 0;
}

int udp_send(network_interface_t* interface, const uint8_t source[4],
    const uint8_t destination[4], uint16_t source_port, uint16_t destination_port,
    const uint8_t* payload, uint16_t payload_length) {
    uint8_t packet[IPV4_MAX_PAYLOAD];
    uint16_t length = (uint16_t)(UDP_HEADER_SIZE + payload_length);
    uint16_t checksum;

    if (interface == 0 || source == 0 || destination == 0 || source_port == 0U
        || destination_port == 0U || (payload_length != 0U && payload == 0)
        || length > sizeof(packet)) {
        return -1;
    }
    write_be16(&packet[0U], source_port);
    write_be16(&packet[2U], destination_port);
    write_be16(&packet[4U], length);
    packet[6U] = 0U;
    packet[7U] = 0U;
    for (uint32_t index = 0U; index < payload_length; index++) {
        packet[UDP_HEADER_SIZE + index] = payload[index];
    }
    checksum = ipv4_transport_checksum(source, destination, UDP_PROTOCOL_NUMBER,
        packet, length);
    write_be16(&packet[6U], checksum == 0U ? 0xFFFFU : checksum);
    return ipv4_send_from(interface, source, destination, UDP_PROTOCOL_NUMBER, packet, length);
}
