#ifndef PRISMOS_NET_TCP_H
#define PRISMOS_NET_TCP_H

#include <stdint.h>

typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_SYN_SENT,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_TIME_WAIT,
    TCP_STATE_RESET
} tcp_state_t;

void tcp_init(void);
void tcp_poll(void);
tcp_state_t tcp_get_state(void);
int tcp_connect(const uint8_t address[4], uint16_t port, uint32_t timeout_ms);
int tcp_send(const uint8_t* data, uint16_t length, uint32_t timeout_ms);
int tcp_read(uint8_t* buffer, uint16_t capacity, uint16_t* out_length);
int tcp_close(uint32_t timeout_ms);

#endif
