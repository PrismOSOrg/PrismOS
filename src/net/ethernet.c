#include "net/ethernet.h"

#include "net/network.h"

#define ETHERNET_MAX_PROTOCOL_HANDLERS 8U

typedef struct {
    uint16_t protocol;
    ethernet_protocol_handler_t handler;
    void* context;
} EthernetProtocolEntry;

static EthernetProtocolEntry protocol_handlers[ETHERNET_MAX_PROTOCOL_HANDLERS];
static uint32_t protocol_handler_count;

static uint16_t ethernet_read_be16(const uint8_t* data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void ethernet_write_be16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

void ethernet_init(void) {
    protocol_handler_count = 0U;
}

int ethernet_register_protocol(uint16_t protocol, ethernet_protocol_handler_t handler, void* context) {
    if (handler == 0 || protocol_handler_count >= ETHERNET_MAX_PROTOCOL_HANDLERS) {
        return -1;
    }
    for (uint32_t index = 0U; index < protocol_handler_count; index++) {
        if (protocol_handlers[index].protocol == protocol) {
            return -1;
        }
    }
    protocol_handlers[protocol_handler_count].protocol = protocol;
    protocol_handlers[protocol_handler_count].handler = handler;
    protocol_handlers[protocol_handler_count].context = context;
    protocol_handler_count++;
    return 0;
}

int ethernet_send(network_interface_t* interface, const uint8_t destination_mac[6],
    uint16_t protocol, const uint8_t* payload, uint16_t payload_length) {
    uint8_t frame[NETWORK_MAX_FRAME_SIZE];
    uint32_t frame_length = ETHERNET_HEADER_SIZE + (uint32_t)payload_length;

    if (interface == 0 || destination_mac == 0
        || (payload_length != 0U && payload == 0)
        || frame_length > NETWORK_MAX_FRAME_SIZE
        || frame_length > (uint32_t)interface->mtu + ETHERNET_HEADER_SIZE) {
        return -1;
    }

    for (uint32_t index = 0U; index < 6U; index++) {
        frame[index] = destination_mac[index];
        frame[6U + index] = interface->mac_address[index];
    }
    ethernet_write_be16(&frame[12U], protocol);
    for (uint32_t index = 0U; index < payload_length; index++) {
        frame[ETHERNET_HEADER_SIZE + index] = payload[index];
    }
    return network_transmit_frame(frame, (uint16_t)frame_length);
}

void ethernet_receive(network_interface_t* interface, const uint8_t* frame, uint16_t length) {
    uint16_t protocol;

    if (interface == 0 || frame == 0 || length < ETHERNET_HEADER_SIZE) {
        return;
    }
    protocol = ethernet_read_be16(&frame[12U]);
    for (uint32_t index = 0U; index < protocol_handler_count; index++) {
        if (protocol_handlers[index].protocol == protocol) {
            protocol_handlers[index].handler(interface, &frame[6U],
                &frame[ETHERNET_HEADER_SIZE],
                (uint16_t)(length - ETHERNET_HEADER_SIZE),
                protocol_handlers[index].context);
            return;
        }
    }
}
