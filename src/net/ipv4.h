#ifndef PRISMOS_NET_IPV4_H
#define PRISMOS_NET_IPV4_H

#include <stdint.h>

#include "net/ethernet.h"
#include "net/network.h"

#define IPV4_PROTOCOL_ICMP 1U
#define IPV4_PROTOCOL_TCP 6U
#define IPV4_PROTOCOL_UDP 17U
#define IPV4_ADDRESS_SIZE 4U
#define IPV4_HEADER_SIZE 20U
#define IPV4_MAX_PAYLOAD (NETWORK_MAX_FRAME_SIZE - ETHERNET_HEADER_SIZE - IPV4_HEADER_SIZE)

typedef void (*ipv4_protocol_handler_t)(network_interface_t* interface,
    const uint8_t source[IPV4_ADDRESS_SIZE], const uint8_t destination[IPV4_ADDRESS_SIZE],
    const uint8_t* payload, uint16_t payload_length, void* context);

typedef struct {
    uint8_t address[IPV4_ADDRESS_SIZE];
    uint8_t netmask[IPV4_ADDRESS_SIZE];
    uint8_t gateway[IPV4_ADDRESS_SIZE];
    uint8_t dns[IPV4_ADDRESS_SIZE];
    uint32_t lease_seconds;
} ipv4_configuration_t;

void ipv4_init(void);
int ipv4_register_protocol(uint8_t protocol, ipv4_protocol_handler_t handler, void* context);
int ipv4_configure(network_interface_t* interface, const ipv4_configuration_t* configuration);
int ipv4_get_configuration(ipv4_configuration_t* configuration);
int ipv4_is_configured(void);
int ipv4_send(network_interface_t* interface, const uint8_t destination[4], uint8_t protocol,
    const uint8_t* payload, uint16_t payload_length);
int ipv4_send_from(network_interface_t* interface, const uint8_t source[4],
    const uint8_t destination[4], uint8_t protocol, const uint8_t* payload,
    uint16_t payload_length);
int ipv4_resolve(const uint8_t address[4], uint32_t timeout_ms, uint8_t out_mac[6]);
uint16_t ipv4_checksum(const uint8_t* data, uint16_t length);
uint16_t ipv4_transport_checksum(const uint8_t source[4], const uint8_t destination[4],
    uint8_t protocol, const uint8_t* segment, uint16_t length);

#endif
