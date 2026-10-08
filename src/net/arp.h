#ifndef PRISMOS_NET_ARP_H
#define PRISMOS_NET_ARP_H

#include <stdint.h>

#include "net/network.h"

void arp_init(void);
int arp_request_ipv4(network_interface_t* interface, const uint8_t sender_ip[4],
    const uint8_t target_ip[4]);
int arp_lookup_ipv4(const uint8_t ip_address[4], uint8_t out_mac[6]);
void arp_learn_ipv4(const uint8_t ip_address[4], const uint8_t mac_address[6]);

#endif
