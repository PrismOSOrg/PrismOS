#ifndef PRISMOS_NET_ETHERNET_H
#define PRISMOS_NET_ETHERNET_H

#include <stdint.h>

#include "net/network.h"

#define ETHERNET_HEADER_SIZE 14U
#define ETHERNET_PROTOCOL_ARP 0x0806U
#define ETHERNET_PROTOCOL_IPV4 0x0800U
#define ETHERNET_PROTOCOL_IPV6 0x86DDU
#define ETHERNET_BROADCAST_ADDRESS {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU}

typedef void (*ethernet_protocol_handler_t)(network_interface_t* interface,
    const uint8_t source_mac[6], const uint8_t* payload, uint16_t payload_length,
    void* context);

void ethernet_init(void);
int ethernet_register_protocol(uint16_t protocol, ethernet_protocol_handler_t handler, void* context);
int ethernet_send(network_interface_t* interface, const uint8_t destination_mac[6],
    uint16_t protocol, const uint8_t* payload, uint16_t payload_length);
void ethernet_receive(network_interface_t* interface, const uint8_t* frame, uint16_t length);

#endif
