#ifndef PRISMOS_NET_DHCP_H
#define PRISMOS_NET_DHCP_H

#include <stdint.h>

typedef enum {
    DHCP_STATE_STOPPED = 0,
    DHCP_STATE_DISCOVERING,
    DHCP_STATE_REQUESTING,
    DHCP_STATE_RENEWING,
    DHCP_STATE_REBINDING,
    DHCP_STATE_BOUND,
    DHCP_STATE_FAILED
} dhcp_state_t;

void dhcp_init(void);
int dhcp_start(void);
void dhcp_poll(void);
dhcp_state_t dhcp_get_state(void);

#endif
