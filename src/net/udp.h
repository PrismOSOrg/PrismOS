#ifndef PRISMOS_NET_UDP_H
#define PRISMOS_NET_UDP_H

#include <stdint.h>

#include "net/network.h"

typedef void (*udp_port_handler_t)(network_interface_t* interface,
    const uint8_t source[4], const uint8_t destination[4], uint16_t source_port,
    uint16_t destination_port, const uint8_t* payload, uint16_t payload_length,
    void* context);

void udp_init(void);
int udp_register_port(uint16_t port, udp_port_handler_t handler, void* context);
int udp_send(network_interface_t* interface, const uint8_t source[4],
    const uint8_t destination[4], uint16_t source_port, uint16_t destination_port,
    const uint8_t* payload, uint16_t payload_length);

#endif
