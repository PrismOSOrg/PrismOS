#ifndef PRISMOS_NET_NETWORK_H
#define PRISMOS_NET_NETWORK_H

#include <stdint.h>

#define NETWORK_MAX_INTERFACES 4U
#define NETWORK_MAX_FRAME_SIZE 1518U
#define NETWORK_RX_QUEUE_SIZE 8U
#define NETWORK_INTERFACE_NAME_SIZE 16U

typedef struct network_interface network_interface_t;

typedef struct {
    int (*transmit)(void* context, const uint8_t* frame, uint16_t length);
    void (*poll)(void* context);
} network_driver_ops_t;

typedef struct {
    uint32_t transmitted_frames;
    uint32_t received_frames;
    uint32_t dropped_frames;
    uint32_t transmit_errors;
    uint32_t receive_errors;
} network_stats_t;

struct network_interface {
    char name[NETWORK_INTERFACE_NAME_SIZE];
    uint8_t mac_address[6];
    uint16_t mtu;
    uint8_t link_up;
    const network_driver_ops_t* operations;
    void* driver_context;
    network_stats_t stats;
};

/* Initializes the protocol core and probes the supported PCI NIC drivers. */
int network_init(void);
int network_interface_count(void);
network_interface_t* network_interface_get(uint32_t index);
network_interface_t* network_default_interface(void);

/* Transmits/receives complete Ethernet frames. Poll regularly to service NICs. */
int network_transmit_frame(const uint8_t* frame, uint16_t length);
int network_receive_frame(uint8_t* buffer, uint16_t capacity, uint16_t* out_length);
void network_poll(void);

/* Driver-facing registration and receive entry point. */
network_interface_t* network_register_interface(const char* name, const uint8_t mac_address[6],
    uint16_t mtu, uint8_t link_up, const network_driver_ops_t* operations, void* context);
int network_submit_received_frame(network_interface_t* interface, const uint8_t* frame,
    uint16_t length);

#endif
