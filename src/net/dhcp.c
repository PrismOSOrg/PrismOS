#include "net/dhcp.h"

#include "debug/log.h"
#include "interrupts/interrupts.h"
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/network.h"
#include "net/udp.h"

#define DHCP_CLIENT_PORT 68U
#define DHCP_SERVER_PORT 67U
#define DHCP_FIXED_HEADER_SIZE 240U
#define DHCP_PACKET_CAPACITY 576U
#define DHCP_MAGIC_COOKIE 0x63825363U
#define DHCP_DISCOVER 1U
#define DHCP_OFFER 2U
#define DHCP_REQUEST 3U
#define DHCP_ACK 5U
#define DHCP_MESSAGE_TYPE 53U
#define DHCP_SERVER_IDENTIFIER 54U
#define DHCP_REQUESTED_ADDRESS 50U
#define DHCP_SUBNET_MASK 1U
#define DHCP_ROUTER 3U
#define DHCP_DNS 6U
#define DHCP_LEASE_TIME 51U
#define DHCP_END 255U
#define DHCP_RETRY_MS 4000U
#define DHCP_MAX_RETRIES 3U

static dhcp_state_t state;
static uint32_t transaction_id;
static uint32_t last_transmit_ms;
static uint32_t lease_acquired_ms;
static uint32_t retry_count;
static uint8_t offered_address[4];
static uint8_t server_address[4];
static network_interface_t* dhcp_interface;

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

static int bytes_equal(const uint8_t* left, const uint8_t* right, uint32_t length) {
    for (uint32_t index = 0U; index < length; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static uint16_t append_option(uint8_t* packet, uint16_t cursor, uint8_t code,
    const uint8_t* data, uint8_t length) {
    packet[cursor++] = code;
    packet[cursor++] = length;
    for (uint32_t index = 0U; index < length; index++) {
        packet[cursor++] = data[index];
    }
    return cursor;
}

static int dhcp_send(uint8_t message_type) {
    static const uint8_t zero_address[4] = {0U, 0U, 0U, 0U};
    static const uint8_t broadcast_address[4] = {255U, 255U, 255U, 255U};
    uint8_t packet[DHCP_PACKET_CAPACITY];
    uint8_t type_value = message_type;
    ipv4_configuration_t current_configuration;
    const uint8_t* source_address = zero_address;
    const uint8_t* destination_address = broadcast_address;
    uint8_t client_identifier[7];
    uint8_t parameter_list[4] = {DHCP_SUBNET_MASK, DHCP_ROUTER, DHCP_DNS, DHCP_LEASE_TIME};
    uint16_t cursor = DHCP_FIXED_HEADER_SIZE;

    if (dhcp_interface == 0) {
        return -1;
    }
    if ((state == DHCP_STATE_RENEWING || state == DHCP_STATE_REBINDING)
        && ipv4_get_configuration(&current_configuration) == 0) {
        source_address = current_configuration.address;
        if (state == DHCP_STATE_RENEWING) {
            destination_address = server_address;
        }
    }
    for (uint32_t index = 0U; index < sizeof(packet); index++) {
        packet[index] = 0U;
    }
    if (state == DHCP_STATE_RENEWING || state == DHCP_STATE_REBINDING) {
        for (uint32_t index = 0U; index < 4U; index++) {
            packet[12U + index] = source_address[index];
        }
    }
    packet[0U] = 1U;
    packet[1U] = 1U;
    packet[2U] = 6U;
    write_be32(&packet[4U], transaction_id);
    if (state == DHCP_STATE_DISCOVERING || state == DHCP_STATE_REQUESTING
        || state == DHCP_STATE_REBINDING) {
        write_be16(&packet[10U], 0x8000U);
    }
    for (uint32_t index = 0U; index < 6U; index++) {
        packet[28U + index] = dhcp_interface->mac_address[index];
        client_identifier[index + 1U] = dhcp_interface->mac_address[index];
    }
    write_be32(&packet[236U], DHCP_MAGIC_COOKIE);
    cursor = append_option(packet, cursor, DHCP_MESSAGE_TYPE, &type_value, 1U);
    if (message_type == DHCP_REQUEST && state == DHCP_STATE_REQUESTING) {
        cursor = append_option(packet, cursor, DHCP_REQUESTED_ADDRESS, offered_address, 4U);
        cursor = append_option(packet, cursor, DHCP_SERVER_IDENTIFIER, server_address, 4U);
    }
    client_identifier[0U] = 1U;
    cursor = append_option(packet, cursor, 61U, client_identifier, 7U);
    cursor = append_option(packet, cursor, 55U, parameter_list, sizeof(parameter_list));
    packet[cursor++] = DHCP_END;
    if (cursor < 300U) {
        cursor = 300U;
    }
    last_transmit_ms = system_uptime_ms();
    return udp_send(dhcp_interface, source_address, destination_address,
        DHCP_CLIENT_PORT, DHCP_SERVER_PORT, packet, cursor);
}

static void dhcp_receive(network_interface_t* interface, const uint8_t source[4],
    const uint8_t destination[4], uint16_t source_port, uint16_t destination_port,
    const uint8_t* packet, uint16_t packet_length, void* context) {
    uint8_t message_type = 0U;
    uint8_t subnet_mask[4] = {255U, 255U, 255U, 0U};
    uint8_t router[4] = {0U, 0U, 0U, 0U};
    uint8_t dns[4] = {0U, 0U, 0U, 0U};
    uint32_t lease_time = 0U;
    uint16_t cursor;
    (void)source;
    (void)destination;
    (void)source_port;
    (void)destination_port;
    (void)context;

    if (interface != dhcp_interface || packet == 0 || packet_length < DHCP_FIXED_HEADER_SIZE
        || packet[0U] != 2U || read_be32(&packet[4U]) != transaction_id
        || !bytes_equal(&packet[28U], interface->mac_address, 6U)
        || read_be32(&packet[236U]) != DHCP_MAGIC_COOKIE) {
        return;
    }
    cursor = DHCP_FIXED_HEADER_SIZE;
    while (cursor < packet_length) {
        uint8_t option = packet[cursor++];
        uint8_t option_length;
        if (option == 0U) {
            continue;
        }
        if (option == DHCP_END) {
            break;
        }
        if (cursor >= packet_length) {
            return;
        }
        option_length = packet[cursor++];
        if ((uint32_t)cursor + option_length > packet_length) {
            return;
        }
        if (option == DHCP_MESSAGE_TYPE && option_length == 1U) {
            message_type = packet[cursor];
        } else if (option == DHCP_SERVER_IDENTIFIER && option_length >= 4U) {
            for (uint32_t i = 0U; i < 4U; i++) server_address[i] = packet[cursor + i];
        } else if (option == DHCP_SUBNET_MASK && option_length >= 4U) {
            for (uint32_t i = 0U; i < 4U; i++) subnet_mask[i] = packet[cursor + i];
        } else if (option == DHCP_ROUTER && option_length >= 4U) {
            for (uint32_t i = 0U; i < 4U; i++) router[i] = packet[cursor + i];
        } else if (option == DHCP_DNS && option_length >= 4U) {
            for (uint32_t i = 0U; i < 4U; i++) dns[i] = packet[cursor + i];
        } else if (option == DHCP_LEASE_TIME && option_length == 4U) {
            lease_time = read_be32(&packet[cursor]);
        }
        cursor = (uint16_t)(cursor + option_length);
    }

    if (message_type == DHCP_OFFER && state == DHCP_STATE_DISCOVERING) {
        for (uint32_t index = 0U; index < 4U; index++) {
            offered_address[index] = packet[16U + index];
        }
        if (bytes_equal(server_address, (const uint8_t[4]){0U, 0U, 0U, 0U}, 4U)) {
            for (uint32_t index = 0U; index < 4U; index++) server_address[index] = source[index];
        }
        state = DHCP_STATE_REQUESTING;
        retry_count = 0U;
        if (dhcp_send(DHCP_REQUEST) != 0) {
            state = DHCP_STATE_FAILED;
            ERROR_LOG("dhcp: request transmission failed");
        } else {
            DEBUG_LOG("dhcp: offer received; request sent");
        }
    } else if (message_type == DHCP_ACK && (state == DHCP_STATE_REQUESTING
        || state == DHCP_STATE_RENEWING || state == DHCP_STATE_REBINDING)) {
        ipv4_configuration_t configuration;
        for (uint32_t i = 0U; i < 4U; i++) {
            configuration.address[i] = packet[16U + i];
            configuration.netmask[i] = subnet_mask[i];
            configuration.gateway[i] = router[i];
            configuration.dns[i] = dns[i];
        }
        configuration.lease_seconds = lease_time;
        if (configuration.address[0] == 0U && configuration.address[1] == 0U
            && configuration.address[2] == 0U && configuration.address[3] == 0U) {
            for (uint32_t i = 0U; i < 4U; i++) configuration.address[i] = offered_address[i];
        }
        if (ipv4_configure(interface, &configuration) == 0) {
            state = DHCP_STATE_BOUND;
            lease_acquired_ms = system_uptime_ms();
            DEBUG_LOG("dhcp: lease acquired");
        } else {
            state = DHCP_STATE_FAILED;
            ERROR_LOG("dhcp: interface configuration failed");
        }
    }
}

void dhcp_init(void) {
    state = DHCP_STATE_STOPPED;
    dhcp_interface = 0;
    retry_count = 0U;
    lease_acquired_ms = 0U;
    offered_address[0] = offered_address[1] = offered_address[2] = offered_address[3] = 0U;
    server_address[0] = server_address[1] = server_address[2] = server_address[3] = 0U;
    (void)udp_register_port(DHCP_CLIENT_PORT, dhcp_receive, 0);
}

int dhcp_start(void) {
    dhcp_interface = network_default_interface();
    if (dhcp_interface == 0 || !dhcp_interface->link_up) {
        state = DHCP_STATE_FAILED;
        return -1;
    }
    transaction_id = 0x50524D00U;
    for (uint32_t index = 0U; index < 6U; index++) {
        transaction_id = (transaction_id * 33U) ^ dhcp_interface->mac_address[index];
    }
    transaction_id ^= system_uptime_ms();
    state = DHCP_STATE_DISCOVERING;
    retry_count = 0U;
    for (uint32_t index = 0U; index < 4U; index++) {
        offered_address[index] = 0U;
        server_address[index] = 0U;
    }
    if (dhcp_send(DHCP_DISCOVER) != 0) {
        state = DHCP_STATE_FAILED;
        return -1;
    }
    DEBUG_LOG("dhcp: discover transmitted");
    return 0;
}

void dhcp_poll(void) {
    uint32_t now = system_uptime_ms();
    if (state == DHCP_STATE_BOUND) {
        ipv4_configuration_t configuration;
        uint32_t elapsed = now - lease_acquired_ms;
        uint32_t lease_ms;
        if (ipv4_get_configuration(&configuration) != 0 || configuration.lease_seconds == 0U) {
            return;
        }
        lease_ms = configuration.lease_seconds > 0xFFFFFFFFU / 1000U
            ? 0xFFFFFFFFU : configuration.lease_seconds * 1000U;
        if (elapsed >= lease_ms) {
            ipv4_configuration_t empty_configuration = {{0U, 0U, 0U, 0U},
                {0U, 0U, 0U, 0U}, {0U, 0U, 0U, 0U}, {0U, 0U, 0U, 0U}, 0U};
            (void)ipv4_configure(dhcp_interface, &empty_configuration);
            state = DHCP_STATE_FAILED;
            ERROR_LOG("dhcp: lease expired");
        } else if (elapsed >= lease_ms / 2U) {
            state = DHCP_STATE_RENEWING;
            retry_count = 0U;
            if (dhcp_send(DHCP_REQUEST) != 0) {
                ERROR_LOG("dhcp: renewal request transmission failed");
            }
        }
        return;
    }
    if (state == DHCP_STATE_RENEWING) {
        ipv4_configuration_t configuration;
        uint32_t elapsed = now - lease_acquired_ms;
        uint32_t lease_ms = 0xFFFFFFFFU;
        if (ipv4_get_configuration(&configuration) == 0
            && configuration.lease_seconds <= 0xFFFFFFFFU / 1000U) {
            lease_ms = configuration.lease_seconds * 1000U;
        }
        if (elapsed >= lease_ms - lease_ms / 8U) {
            state = DHCP_STATE_REBINDING;
            retry_count = 0U;
            if (dhcp_send(DHCP_REQUEST) != 0) {
                ERROR_LOG("dhcp: rebinding request transmission failed");
            }
            return;
        }
    }
    if ((state != DHCP_STATE_DISCOVERING && state != DHCP_STATE_REQUESTING
        && state != DHCP_STATE_RENEWING && state != DHCP_STATE_REBINDING)
        || (uint32_t)(now - last_transmit_ms) < DHCP_RETRY_MS) {
        return;
    }
    if (retry_count >= DHCP_MAX_RETRIES
        && (state == DHCP_STATE_DISCOVERING || state == DHCP_STATE_REQUESTING)) {
        state = DHCP_STATE_FAILED;
        ERROR_LOG("dhcp: lease negotiation timed out");
        return;
    }
    retry_count++;
    if (dhcp_send(state == DHCP_STATE_DISCOVERING ? DHCP_DISCOVER : DHCP_REQUEST) != 0) {
        state = DHCP_STATE_FAILED;
        ERROR_LOG("dhcp: retry transmission failed");
    }
}

dhcp_state_t dhcp_get_state(void) {
    return state;
}
