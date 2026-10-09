#include "net/tcp.h"

#include "debug/log.h"
#include "interrupts/interrupts.h"
#include "net/ipv4.h"
#include "net/network.h"

#define TCP_HEADER_SIZE 20U
#define TCP_MAX_PAYLOAD 512U
#define TCP_PORT_EPHEMERAL_BASE 49152U
#define TCP_RETRANSMIT_MS 500U
#define TCP_MAX_RETRIES 4U
#define TCP_FLAG_FIN 0x01U
#define TCP_FLAG_SYN 0x02U
#define TCP_FLAG_RST 0x04U
#define TCP_FLAG_PSH 0x08U
#define TCP_FLAG_ACK 0x10U
#define TCP_OUTSTANDING_CAPACITY (TCP_HEADER_SIZE + TCP_MAX_PAYLOAD)

typedef struct {
    tcp_state_t state;
    uint8_t peer_address[4];
    uint16_t local_port;
    uint16_t peer_port;
    uint32_t send_unacknowledged;
    uint32_t send_next;
    uint32_t receive_next;
    uint32_t last_transmit_ms;
    uint32_t retries;
    uint8_t outstanding;
    uint16_t outstanding_length;
    uint8_t outstanding_packet[TCP_OUTSTANDING_CAPACITY];
    uint8_t receive_buffer[TCP_MAX_PAYLOAD];
    uint16_t receive_length;
} TcpConnection;

static TcpConnection connection;
static uint16_t next_ephemeral_port = TCP_PORT_EPHEMERAL_BASE;

static uint16_t read_be16(const uint8_t* data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
        | ((uint32_t)data[2] << 8) | data[3];
}

static void write_be16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_be32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static int address_equal(const uint8_t* left, const uint8_t* right) {
    for (uint32_t index = 0U; index < 4U; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static void tcp_build_segment(uint8_t* packet, uint16_t* out_length, uint8_t flags,
    const uint8_t* payload, uint16_t payload_length, uint32_t sequence, uint32_t acknowledgement) {
    ipv4_configuration_t configuration;
    uint16_t checksum;
    uint16_t length = (uint16_t)(TCP_HEADER_SIZE + payload_length);

    (void)ipv4_get_configuration(&configuration);
    for (uint32_t index = 0U; index < length; index++) {
        packet[index] = 0U;
    }
    write_be16(&packet[0U], connection.local_port);
    write_be16(&packet[2U], connection.peer_port);
    write_be32(&packet[4U], sequence);
    write_be32(&packet[8U], acknowledgement);
    packet[12U] = 0x50U;
    packet[13U] = flags;
    write_be16(&packet[14U], (uint16_t)(TCP_MAX_PAYLOAD - connection.receive_length));
    for (uint32_t index = 0U; index < payload_length; index++) {
        packet[TCP_HEADER_SIZE + index] = payload[index];
    }
    checksum = ipv4_transport_checksum(configuration.address, connection.peer_address,
        IPV4_PROTOCOL_TCP, packet, length);
    write_be16(&packet[16U], checksum);
    *out_length = length;
}

static int tcp_transmit_packet(const uint8_t* packet, uint16_t length) {
    return ipv4_send(network_default_interface(), connection.peer_address,
        IPV4_PROTOCOL_TCP, packet, length);
}

static int tcp_send_control(uint8_t flags, int track) {
    uint16_t length;
    uint32_t sequence = connection.send_next;
    tcp_build_segment(connection.outstanding_packet, &length, flags, 0, 0U,
        sequence, connection.receive_next);
    if (tcp_transmit_packet(connection.outstanding_packet, length) != 0) {
        return -1;
    }
    if (track) {
        connection.outstanding_length = length;
        connection.outstanding = 1U;
        connection.last_transmit_ms = system_uptime_ms();
        connection.retries = 0U;
        connection.send_next += (uint32_t)(((flags & TCP_FLAG_SYN) != 0U)
            + ((flags & TCP_FLAG_FIN) != 0U));
    }
    return 0;
}

static void tcp_send_ack(void) {
    uint8_t packet[TCP_HEADER_SIZE];
    uint16_t length;
    tcp_build_segment(packet, &length, TCP_FLAG_ACK, 0, 0U,
        connection.send_next, connection.receive_next);
    (void)tcp_transmit_packet(packet, length);
}

static void tcp_receive(network_interface_t* interface, const uint8_t source[4],
    const uint8_t destination[4], const uint8_t* packet, uint16_t packet_length,
    void* context) {
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t header_length;
    uint16_t data_length;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint8_t flags;
    (void)interface;
    (void)destination;
    (void)context;

    if (packet == 0 || packet_length < TCP_HEADER_SIZE
        || ipv4_transport_checksum(source, destination, IPV4_PROTOCOL_TCP,
            packet, packet_length) != 0U) {
        return;
    }
    source_port = read_be16(&packet[0U]);
    destination_port = read_be16(&packet[2U]);
    if (connection.state == TCP_STATE_CLOSED || destination_port != connection.local_port
        || source_port != connection.peer_port || !address_equal(source, connection.peer_address)) {
        return;
    }
    header_length = (uint16_t)((packet[12U] >> 4) * 4U);
    if (header_length < TCP_HEADER_SIZE || header_length > packet_length) {
        return;
    }
    data_length = (uint16_t)(packet_length - header_length);
    sequence = read_be32(&packet[4U]);
    acknowledgement = read_be32(&packet[8U]);
    flags = packet[13U];
    if ((flags & TCP_FLAG_RST) != 0U) {
        connection.state = TCP_STATE_RESET;
        connection.outstanding = 0U;
        return;
    }

    if (connection.state == TCP_STATE_SYN_SENT && (flags & (TCP_FLAG_SYN | TCP_FLAG_ACK))
        == (TCP_FLAG_SYN | TCP_FLAG_ACK) && acknowledgement == connection.send_next) {
        connection.receive_next = sequence + 1U;
        connection.send_unacknowledged = acknowledgement;
        connection.outstanding = 0U;
        connection.state = TCP_STATE_ESTABLISHED;
        tcp_send_ack();
        DEBUG_LOG("tcp: connection established");
        return;
    }

    if ((flags & TCP_FLAG_ACK) != 0U
        && (int32_t)(acknowledgement - connection.send_unacknowledged) >= 0
        && (int32_t)(connection.send_next - acknowledgement) >= 0) {
        connection.send_unacknowledged = acknowledgement;
        if (connection.outstanding && acknowledgement == connection.send_next) {
            connection.outstanding = 0U;
        }
    }
    if (sequence != connection.receive_next) {
        if (data_length != 0U || (flags & TCP_FLAG_FIN) != 0U) {
            tcp_send_ack();
        }
        return;
    }
    if (data_length != 0U) {
        uint16_t free_space = (uint16_t)(TCP_MAX_PAYLOAD - connection.receive_length);
        uint16_t accepted = data_length < free_space ? data_length : free_space;
        for (uint32_t index = 0U; index < accepted; index++) {
            connection.receive_buffer[connection.receive_length + index] = packet[header_length + index];
        }
        connection.receive_length = (uint16_t)(connection.receive_length + accepted);
        connection.receive_next += accepted;
        tcp_send_ack();
        DEBUG_LOG("tcp: peer data received");
    }
    if ((flags & TCP_FLAG_FIN) != 0U) {
        connection.receive_next++;
        connection.state = connection.state == TCP_STATE_FIN_WAIT
            ? TCP_STATE_TIME_WAIT : TCP_STATE_CLOSE_WAIT;
        tcp_send_ack();
    }
}

void tcp_init(void) {
    connection.state = TCP_STATE_CLOSED;
    connection.outstanding = 0U;
    connection.receive_length = 0U;
    next_ephemeral_port = TCP_PORT_EPHEMERAL_BASE;
    (void)ipv4_register_protocol(IPV4_PROTOCOL_TCP, tcp_receive, 0);
}

void tcp_poll(void) {
    if (!connection.outstanding
        || (uint32_t)(system_uptime_ms() - connection.last_transmit_ms) < TCP_RETRANSMIT_MS) {
        return;
    }
    if (connection.retries >= TCP_MAX_RETRIES) {
        connection.outstanding = 0U;
        connection.state = TCP_STATE_RESET;
        return;
    }
    if (tcp_transmit_packet(connection.outstanding_packet, connection.outstanding_length) == 0) {
        connection.retries++;
        connection.last_transmit_ms = system_uptime_ms();
    }
}

tcp_state_t tcp_get_state(void) {
    return connection.state;
}

int tcp_connect(const uint8_t address[4], uint16_t port, uint32_t timeout_ms) {
    network_interface_t* interface = network_default_interface();
    uint8_t mac_address[6];
    uint32_t started;

    if (address == 0 || port == 0U || interface == 0 || !interface->link_up
        || (connection.state != TCP_STATE_CLOSED && connection.state != TCP_STATE_RESET)) {
        return -1;
    }
    if (ipv4_resolve(address, timeout_ms, mac_address) != 0) {
        return -2;
    }
    connection.state = TCP_STATE_SYN_SENT;
    connection.peer_address[0] = address[0];
    connection.peer_address[1] = address[1];
    connection.peer_address[2] = address[2];
    connection.peer_address[3] = address[3];
    connection.peer_port = port;
    connection.local_port = next_ephemeral_port++;
    if (next_ephemeral_port < TCP_PORT_EPHEMERAL_BASE) {
        next_ephemeral_port = TCP_PORT_EPHEMERAL_BASE;
    }
    connection.send_unacknowledged = (system_uptime_ms() << 12) ^ 0x50524953U;
    connection.send_next = connection.send_unacknowledged;
    connection.receive_next = 0U;
    connection.receive_length = 0U;
    if (tcp_send_control(TCP_FLAG_SYN, 1) != 0) {
        connection.state = TCP_STATE_CLOSED;
        return -3;
    }
    started = system_uptime_ms();
    while (connection.state == TCP_STATE_SYN_SENT
        && (uint32_t)(system_uptime_ms() - started) < timeout_ms) {
        network_poll();
        __asm__ volatile ("hlt");
    }
    if (connection.state == TCP_STATE_ESTABLISHED) {
        return 0;
    }
    connection.outstanding = 0U;
    if (connection.state == TCP_STATE_SYN_SENT) {
        connection.state = TCP_STATE_CLOSED;
        return -4;
    }
    return -5;
}

int tcp_send(const uint8_t* data, uint16_t length, uint32_t timeout_ms) {
    uint32_t started;
    uint16_t packet_length;

    if (connection.state != TCP_STATE_ESTABLISHED || data == 0 || length == 0U
        || length > TCP_MAX_PAYLOAD || connection.outstanding) {
        return -1;
    }
    tcp_build_segment(connection.outstanding_packet, &packet_length,
        TCP_FLAG_ACK | TCP_FLAG_PSH, data, length, connection.send_next,
        connection.receive_next);
    if (tcp_transmit_packet(connection.outstanding_packet, packet_length) != 0) {
        return -2;
    }
    connection.outstanding_length = packet_length;
    connection.outstanding = 1U;
    connection.last_transmit_ms = system_uptime_ms();
    connection.retries = 0U;
    connection.send_next += length;
    started = system_uptime_ms();
    while (connection.outstanding && connection.state == TCP_STATE_ESTABLISHED
        && (uint32_t)(system_uptime_ms() - started) < timeout_ms) {
        network_poll();
        __asm__ volatile ("hlt");
    }
    return connection.outstanding ? -3 : 0;
}

int tcp_read(uint8_t* buffer, uint16_t capacity, uint16_t* out_length) {
    uint16_t length;
    if (buffer == 0 || out_length == 0 || capacity == 0U || connection.receive_length == 0U) {
        return -1;
    }
    length = capacity < connection.receive_length ? capacity : connection.receive_length;
    for (uint32_t index = 0U; index < length; index++) {
        buffer[index] = connection.receive_buffer[index];
    }
    for (uint32_t index = length; index < connection.receive_length; index++) {
        connection.receive_buffer[index - length] = connection.receive_buffer[index];
    }
    connection.receive_length = (uint16_t)(connection.receive_length - length);
    *out_length = length;
    if (connection.state == TCP_STATE_ESTABLISHED || connection.state == TCP_STATE_CLOSE_WAIT) {
        tcp_send_ack();
    }
    return 0;
}

int tcp_read_wait(uint8_t* buffer, uint16_t capacity, uint16_t* out_length,
    uint32_t timeout_ms) {
    uint32_t started;
    if (buffer == 0 || out_length == 0 || capacity == 0U) {
        return -1;
    }
    started = system_uptime_ms();
    while (connection.receive_length == 0U) {
        if (connection.state == TCP_STATE_CLOSE_WAIT || connection.state == TCP_STATE_RESET
            || connection.state == TCP_STATE_TIME_WAIT || connection.state == TCP_STATE_CLOSED) {
            return -2;
        }
        if ((uint32_t)(system_uptime_ms() - started) >= timeout_ms) {
            return -3;
        }
        network_poll();
        __asm__ volatile ("hlt");
    }
    return tcp_read(buffer, capacity, out_length);
}

int tcp_close(uint32_t timeout_ms) {
    uint32_t started;
    if (connection.state == TCP_STATE_CLOSE_WAIT) {
        connection.state = TCP_STATE_FIN_WAIT;
    } else if (connection.state != TCP_STATE_ESTABLISHED) {
        return -1;
    } else {
        connection.state = TCP_STATE_FIN_WAIT;
    }
    if (tcp_send_control(TCP_FLAG_FIN | TCP_FLAG_ACK, 1) != 0) {
        connection.state = TCP_STATE_RESET;
        return -2;
    }
    started = system_uptime_ms();
    while (connection.outstanding && connection.state != TCP_STATE_RESET
        && (uint32_t)(system_uptime_ms() - started) < timeout_ms) {
        network_poll();
        __asm__ volatile ("hlt");
    }
    if (connection.state != TCP_STATE_RESET) {
        connection.state = TCP_STATE_CLOSED;
    }
    connection.outstanding = 0U;
    return 0;
}
