#ifndef PRISMOS_NET_ICMP_H
#define PRISMOS_NET_ICMP_H

#include <stdint.h>

void icmp_init(void);
int icmp_ping(const uint8_t destination[4], uint32_t timeout_ms, uint32_t* round_trip_ms);

#endif
