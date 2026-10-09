#ifndef PRISMOS_NET_DNS_H
#define PRISMOS_NET_DNS_H

#include <stdint.h>

void dns_init(void);
int dns_resolve_ipv4(const char* hostname, uint32_t timeout_ms, uint8_t out_address[4]);

#endif
