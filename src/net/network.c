#include "net/network.h"

#include "net/arp.h"
#include "net/dhcp.h"
#include "net/dns.h"
#include "net/ethernet.h"
#include "net/drivers/e1000.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/tcp.h"
#include "net/udp.h"

#define NETWORK_MIN_FRAME_SIZE 14U

typedef struct {
    network_interface_t* interface;
    uint16_t length;
    uint8_t data[NETWORK_MAX_FRAME_SIZE];
} NetworkReceiveEntry;

static network_interface_t interfaces[NETWORK_MAX_INTERFACES];
static uint32_t interface_count;
static NetworkReceiveEntry receive_queue[NETWORK_RX_QUEUE_SIZE];
static uint32_t receive_head;
static uint32_t receive_tail;
static uint32_t receive_count;

static void network_copy(uint8_t* destination, const uint8_t* source, uint32_t count) {
    for (uint32_t index = 0U; index < count; index++) {
        destination[index] = source[index];
    }
}

static void network_copy_name(char* destination, const char* source) {
    uint32_t index = 0U;
    while (source[index] != '\0' && index + 1U < NETWORK_INTERFACE_NAME_SIZE) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

int network_init(void) {
    interface_count = 0U;
    receive_head = 0U;
    receive_tail = 0U;
    receive_count = 0U;
    ethernet_init();
    arp_init();
    ipv4_init();
    udp_init();
    dns_init();
    dhcp_init();
    icmp_init();
    tcp_init();

    if (e1000_driver_init() != 0 || interface_count == 0U) {
        return -1;
    }
    (void)dhcp_start();
    return 0;
}

network_interface_t* network_register_interface(const char* name, const uint8_t mac_address[6],
    uint16_t mtu, uint8_t link_up, const network_driver_ops_t* operations, void* context) {
    network_interface_t* interface;

    if (name == 0 || mac_address == 0 || operations == 0 || operations->transmit == 0
        || operations->poll == 0 || mtu == 0U || interface_count >= NETWORK_MAX_INTERFACES) {
        return 0;
    }

    interface = &interfaces[interface_count++];
    network_copy_name(interface->name, name);
    network_copy(interface->mac_address, mac_address, 6U);
    interface->mtu = mtu;
    interface->link_up = link_up != 0U;
    interface->operations = operations;
    interface->driver_context = context;
    interface->stats.transmitted_frames = 0U;
    interface->stats.received_frames = 0U;
    interface->stats.dropped_frames = 0U;
    interface->stats.transmit_errors = 0U;
    interface->stats.receive_errors = 0U;
    return interface;
}

int network_interface_count(void) {
    return (int)interface_count;
}

network_interface_t* network_interface_get(uint32_t index) {
    return index < interface_count ? &interfaces[index] : 0;
}

network_interface_t* network_default_interface(void) {
    return interface_count == 0U ? 0 : &interfaces[0];
}

int network_transmit_frame(const uint8_t* frame, uint16_t length) {
    network_interface_t* interface = network_default_interface();

    if (interface == 0 || frame == 0 || length < NETWORK_MIN_FRAME_SIZE
        || length > NETWORK_MAX_FRAME_SIZE || length > interface->mtu + ETHERNET_HEADER_SIZE) {
        return -1;
    }
    if (!interface->link_up || interface->operations->transmit(interface->driver_context,
        frame, length) != 0) {
        interface->stats.transmit_errors++;
        return -1;
    }
    interface->stats.transmitted_frames++;
    return 0;
}

int network_submit_received_frame(network_interface_t* interface, const uint8_t* frame,
    uint16_t length) {
    NetworkReceiveEntry* entry;
    int registered = 0;

    for (uint32_t index = 0U; index < interface_count; index++) {
        if (interface == &interfaces[index]) {
            registered = 1;
            break;
        }
    }
    if (!registered || frame == 0 || length < NETWORK_MIN_FRAME_SIZE
        || length > NETWORK_MAX_FRAME_SIZE) {
        if (registered) {
            interface->stats.receive_errors++;
        }
        return -1;
    }
    if (receive_count >= NETWORK_RX_QUEUE_SIZE) {
        interface->stats.dropped_frames++;
        return -1;
    }

    entry = &receive_queue[receive_head];
    entry->interface = interface;
    entry->length = length;
    network_copy(entry->data, frame, length);
    receive_head = (receive_head + 1U) % NETWORK_RX_QUEUE_SIZE;
    receive_count++;
    interface->stats.received_frames++;

    ethernet_receive(interface, entry->data, entry->length);
    return 0;
}

int network_receive_frame(uint8_t* buffer, uint16_t capacity, uint16_t* out_length) {
    NetworkReceiveEntry* entry;

    if (buffer == 0 || out_length == 0 || receive_count == 0U) {
        return -1;
    }
    entry = &receive_queue[receive_tail];
    if (capacity < entry->length) {
        return -2;
    }
    network_copy(buffer, entry->data, entry->length);
    *out_length = entry->length;
    receive_tail = (receive_tail + 1U) % NETWORK_RX_QUEUE_SIZE;
    receive_count--;
    return 0;
}

void network_poll(void) {
    for (uint32_t index = 0U; index < interface_count; index++) {
        interfaces[index].operations->poll(interfaces[index].driver_context);
    }
    dhcp_poll();
    tcp_poll();
}
