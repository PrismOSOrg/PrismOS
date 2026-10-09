#include "apps/network_manager_app.h"

#include <stdint.h>

#include "display/console.h"
#include "input/keyboard.h"
#include "interrupts/interrupts.h"
#include "net/dhcp.h"
#include "net/dns.h"
#include "net/http.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/network.h"
#include "net/tcp.h"

#define NETWORK_UI_MIN_COLUMNS 76U
#define NETWORK_UI_MIN_ROWS 23U
#define NETWORK_UI_INPUT_CAPACITY 96U
#define NETWORK_UI_STATUS_CAPACITY 112U
#define NETWORK_UI_MENU_FIRST_ROW 10U
#define NETWORK_UI_MENU_COUNT 10U
#define NETWORK_UI_READ_CAPACITY 384U
#define NETWORK_UI_READ_ROWS 6U
#define NETWORK_UI_READ_COLUMNS 64U
#define NETWORK_UI_DIRTY_ADAPTER 0x01U
#define NETWORK_UI_DIRTY_IP 0x02U
#define NETWORK_UI_DIRTY_PROTOCOLS 0x04U
#define NETWORK_UI_DIRTY_STATS 0x08U
#define NETWORK_UI_DIRTY_ALL 0x0FU

typedef enum {
    NETWORK_MODAL_NONE = 0,
    NETWORK_MODAL_PING_ADDRESS,
    NETWORK_MODAL_TCP_ADDRESS,
    NETWORK_MODAL_TCP_PORT,
    NETWORK_MODAL_TCP_SEND,
    NETWORK_MODAL_TCP_READ,
    NETWORK_MODAL_DNS_HOSTNAME,
    NETWORK_MODAL_DNS_RESULT,
    NETWORK_MODAL_HTTP_HOSTNAME,
    NETWORK_MODAL_HTTP_PATH,
    NETWORK_MODAL_HTTP_PORT,
    NETWORK_MODAL_HTTP_RESULT,
} NetworkModal;

typedef struct {
    uint32_t columns;
    uint32_t rows;
    uint32_t selected_action;
    uint32_t input_length;
    char input[NETWORK_UI_INPUT_CAPACITY];
    char status[NETWORK_UI_STATUS_CAPACITY];
    VGA_Color status_color;
    NetworkModal modal;
    uint8_t target_address[4];
    uint16_t target_port;
    char target_hostname[NETWORK_UI_INPUT_CAPACITY];
    char target_path[NETWORK_UI_INPUT_CAPACITY];
    char http_content_type[HTTP_CONTENT_TYPE_SIZE];
    uint16_t http_status_code;
    uint8_t received[NETWORK_UI_READ_CAPACITY];
    uint16_t received_length;
    uint32_t last_draw_ms;
    uint32_t shown_transmitted;
    uint32_t shown_received;
    uint32_t shown_dropped;
    uint32_t shown_errors;
    uint8_t shown_link;
    uint8_t shown_configured;
    dhcp_state_t shown_dhcp;
    tcp_state_t shown_tcp;
    ipv4_configuration_t shown_configuration;
    uint8_t snapshot_valid;
    uint8_t status_dirty;
    uint8_t modal_dirty;
    uint32_t full_draw_generation;
    int running;
} NetworkUiState;

static void ui_copy(char* destination, uint32_t capacity, const char* source) {
    uint32_t index = 0U;
    if (capacity == 0U) {
        return;
    }
    while (source[index] != '\0' && index + 1U < capacity) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

static void ui_status(NetworkUiState* state, const char* message, VGA_Color color) {
    ui_copy(state->status, sizeof(state->status), message);
    state->status_color = color;
    state->status_dirty = 1U;
}

static void ui_write_clipped(const char* text, uint32_t width) {
    for (uint32_t index = 0U; text[index] != '\0' && index < width; index++) {
        console_write_char(text[index]);
    }
}

static void ui_fill_row(uint32_t row, VGA_Color foreground, VGA_Color background) {
    console_set_color(foreground, background);
    console_clear_row((int)row);
}

static void ui_clear_row(uint32_t row) {
    ui_fill_row(row, COLOR_LIGHT_GRAY, COLOR_BLACK);
}

static void ui_row(uint32_t row, VGA_Color foreground, VGA_Color background, const char* text) {
    ui_fill_row(row, foreground, background);
    console_set_cursor(0, (int)row);
    ui_write_clipped(text, console_get_framebuffer_width() / 8U);
}

static void ui_draw_menu_item(const NetworkUiState* state, uint32_t index,
    const char* shortcut, const char* title, const char* detail) {
    uint32_t row = NETWORK_UI_MENU_FIRST_ROW + index;
    VGA_Color foreground = state->selected_action == index ? COLOR_WHITE : COLOR_LIGHT_GRAY;
    VGA_Color background = state->selected_action == index ? COLOR_BLUE : COLOR_BLACK;

    ui_fill_row(row, foreground, background);
    console_set_cursor(2, (int)row);
    console_write(state->selected_action == index ? "> [" : "  [");
    console_write(shortcut);
    console_write("] ");
    console_write(title);
    if (state->columns > 58U) {
        console_set_color(state->selected_action == index ? COLOR_LIGHT_CYAN : COLOR_DARK_GRAY,
            background);
        console_set_cursor(38, (int)row);
        ui_write_clipped(detail, state->columns - 40U);
    }
}

static void ui_draw_menu_selection(const NetworkUiState* state, uint32_t index) {
    static const char* shortcuts[NETWORK_UI_MENU_COUNT] = {
        "1/G", "2/P", "3/D", "4/T", "5/S", "6/R", "7/C", "8/U", "9/N", "0/H"
    };
    static const char* titles[NETWORK_UI_MENU_COUNT] = {
        "Ping gateway", "Ping an address", "Renew network settings", "Connect to a TCP service",
        "Send TCP text", "Read TCP response", "Close TCP connection", "Refresh dashboard",
        "Resolve a hostname", "Fetch an HTTP page"
    };
    static const char* details[NETWORK_UI_MENU_COUNT] = {
        "Check the router responds to ICMP", "Enter an IPv4 destination", "Ask DHCP for a fresh lease",
        "Enter remote IPv4 address and port", "Send up to 95 characters on the connection",
        "Display data received from the peer", "Send a graceful close", "Update link state and counters",
        "Look up a DNS name using DHCP DNS", "Request a page over plain HTTP"
    };
    ui_draw_menu_item(state, index, shortcuts[index], titles[index], details[index]);
}

static void ui_print_ipv4(const uint8_t address[4]) {
    for (uint32_t index = 0U; index < 4U; index++) {
        if (index != 0U) {
            console_write_char('.');
        }
        console_write_uint(address[index]);
    }
}

static void ui_print_mac(const uint8_t address[6]) {
    static const char digits[] = "0123456789ABCDEF";
    for (uint32_t index = 0U; index < 6U; index++) {
        if (index != 0U) {
            console_write_char(':');
        }
        console_write_char(digits[address[index] >> 4]);
        console_write_char(digits[address[index] & 0x0FU]);
    }
}

static const char* ui_dhcp_state_name(dhcp_state_t state) {
    switch (state) {
        case DHCP_STATE_DISCOVERING: return "Discovering";
        case DHCP_STATE_REQUESTING: return "Requesting";
        case DHCP_STATE_RENEWING: return "Renewing";
        case DHCP_STATE_REBINDING: return "Rebinding";
        case DHCP_STATE_BOUND: return "Connected";
        case DHCP_STATE_FAILED: return "Needs attention";
        default: return "Not started";
    }
}

static const char* ui_tcp_state_name(tcp_state_t state) {
    switch (state) {
        case TCP_STATE_SYN_SENT: return "Connecting";
        case TCP_STATE_ESTABLISHED: return "Connected";
        case TCP_STATE_FIN_WAIT: return "Closing";
        case TCP_STATE_CLOSE_WAIT: return "Peer closed";
        case TCP_STATE_TIME_WAIT: return "Finishing close";
        case TCP_STATE_RESET: return "Reset by peer";
        default: return "Disconnected";
    }
}

static int ui_ipv4_configuration_equal(const ipv4_configuration_t* left,
    const ipv4_configuration_t* right) {
    for (uint32_t index = 0U; index < 4U; index++) {
        if (left->address[index] != right->address[index]
            || left->netmask[index] != right->netmask[index]
            || left->gateway[index] != right->gateway[index]
            || left->dns[index] != right->dns[index]) {
            return 0;
        }
    }
    return 1;
}

static uint32_t ui_connection_change_mask(const NetworkUiState* state) {
    network_interface_t* interface = network_default_interface();
    ipv4_configuration_t configuration = {0};
    int configured = ipv4_get_configuration(&configuration) == 0;
    uint32_t transmitted = interface != 0 ? interface->stats.transmitted_frames : 0U;
    uint32_t received = interface != 0 ? interface->stats.received_frames : 0U;
    uint32_t dropped = interface != 0 ? interface->stats.dropped_frames : 0U;
    uint32_t errors = interface != 0
        ? interface->stats.transmit_errors + interface->stats.receive_errors : 0U;
    uint8_t link = interface != 0 ? interface->link_up : 0U;

    uint32_t changed = 0U;
    if (!state->snapshot_valid) {
        return NETWORK_UI_DIRTY_ALL;
    }
    if (state->shown_link != link) {
        changed |= NETWORK_UI_DIRTY_ADAPTER;
    }
    if (state->shown_configured != (uint8_t)configured
        || (configured && !ui_ipv4_configuration_equal(&state->shown_configuration, &configuration))) {
        changed |= NETWORK_UI_DIRTY_IP;
    }
    if (state->shown_configured != (uint8_t)configured
        || (configured && state->shown_configuration.lease_seconds != configuration.lease_seconds)
        || state->shown_dhcp != dhcp_get_state() || state->shown_tcp != tcp_get_state()) {
        changed |= NETWORK_UI_DIRTY_PROTOCOLS;
    }
    if (state->shown_transmitted != transmitted || state->shown_received != received
        || state->shown_dropped != dropped || state->shown_errors != errors) {
        changed |= NETWORK_UI_DIRTY_STATS;
    }
    return changed;
}

static void ui_draw_connection_rows(NetworkUiState* state, uint32_t changed) {
    network_interface_t* interface = network_default_interface();
    ipv4_configuration_t configuration = {0};
    int configured = ipv4_get_configuration(&configuration) == 0;
    dhcp_state_t dhcp_state = dhcp_get_state();
    tcp_state_t tcp_state = tcp_get_state();

    if ((changed & NETWORK_UI_DIRTY_ADAPTER) != 0U) {
        ui_fill_row(4U, COLOR_LIGHT_GRAY, COLOR_BLACK);
        console_set_cursor(2, 4);
        if (interface != 0) {
            console_write(interface->name);
            console_write(interface->link_up ? "   |   Link up   |   MAC " : "   |   Link down   |   MAC ");
            ui_print_mac(interface->mac_address);
            console_write("   |   MTU ");
            console_write_uint(interface->mtu);
        } else {
            console_write("No supported Ethernet adapter detected. Use QEMU e1000 with run-net.");
        }
    }

    if ((changed & NETWORK_UI_DIRTY_IP) != 0U) {
        ui_fill_row(5U, COLOR_LIGHT_GRAY, COLOR_BLACK);
        console_set_cursor(2, 5);
        console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        console_write("IPv4 address ");
        console_set_color(configured ? COLOR_WHITE : COLOR_DARK_GRAY, COLOR_BLACK);
        if (configured) ui_print_ipv4(configuration.address); else console_write("Not configured");
        console_set_cursor(38, 5);
        console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        console_write("Subnet mask ");
        console_set_color(configured ? COLOR_WHITE : COLOR_DARK_GRAY, COLOR_BLACK);
        if (configured) ui_print_ipv4(configuration.netmask); else console_write("--");

        ui_fill_row(6U, COLOR_LIGHT_GRAY, COLOR_BLACK);
        console_set_cursor(2, 6);
        console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        console_write("Gateway ");
        console_set_color(configured ? COLOR_WHITE : COLOR_DARK_GRAY, COLOR_BLACK);
        if (configured) ui_print_ipv4(configuration.gateway); else console_write("--");
        console_set_cursor(38, 6);
        console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        console_write("DNS ");
        console_set_color(configured ? COLOR_WHITE : COLOR_DARK_GRAY, COLOR_BLACK);
        if (configured) ui_print_ipv4(configuration.dns); else console_write("--");
    }

    if ((changed & NETWORK_UI_DIRTY_PROTOCOLS) != 0U) {
        ui_fill_row(7U, COLOR_LIGHT_GRAY, COLOR_BLACK);
        console_set_cursor(2, 7);
        console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        console_write("DHCP ");
        console_set_color(dhcp_state == DHCP_STATE_BOUND ? COLOR_LIGHT_GREEN : COLOR_YELLOW,
            COLOR_BLACK);
        console_write(ui_dhcp_state_name(dhcp_state));
        if (configured && configuration.lease_seconds != 0U) {
            console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
            console_write("   |   Lease ");
            console_write_uint(configuration.lease_seconds);
            console_write(" sec");
        }
        console_set_cursor(58, 7);
        console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        console_write("TCP ");
        console_set_color(tcp_state == TCP_STATE_ESTABLISHED ? COLOR_LIGHT_GREEN : COLOR_LIGHT_GRAY,
            COLOR_BLACK);
        console_write(ui_tcp_state_name(tcp_state));
    }

    if ((changed & NETWORK_UI_DIRTY_STATS) != 0U) {
        ui_fill_row(8U, COLOR_LIGHT_GRAY, COLOR_BLACK);
        console_set_cursor(2, 8);
        console_set_color(COLOR_DARK_GRAY, COLOR_BLACK);
        console_write("Frames   TX ");
        console_write_uint(interface != 0 ? interface->stats.transmitted_frames : 0U);
        console_write("   RX ");
        console_write_uint(interface != 0 ? interface->stats.received_frames : 0U);
        console_write("   Dropped ");
        console_write_uint(interface != 0 ? interface->stats.dropped_frames : 0U);
        console_write("   Errors ");
        console_write_uint(interface != 0
            ? interface->stats.transmit_errors + interface->stats.receive_errors : 0U);
    }

    state->shown_link = interface != 0 ? interface->link_up : 0U;
    state->shown_transmitted = interface != 0 ? interface->stats.transmitted_frames : 0U;
    state->shown_received = interface != 0 ? interface->stats.received_frames : 0U;
    state->shown_dropped = interface != 0 ? interface->stats.dropped_frames : 0U;
    state->shown_errors = interface != 0
        ? interface->stats.transmit_errors + interface->stats.receive_errors : 0U;
    state->shown_configuration = configuration;
    state->shown_configured = (uint8_t)configured;
    state->shown_dhcp = dhcp_state;
    state->shown_tcp = tcp_state;
    state->snapshot_valid = 1U;
}

static void ui_draw_status(const NetworkUiState* state) {
    ui_row(state->rows - 3U, state->status_color, COLOR_BLACK, state->status);
}

static void ui_draw_dashboard(NetworkUiState* state) {
    console_set_cursor_visible(0);
    ui_row(0U, COLOR_WHITE, COLOR_BLUE, " PrismOS     NETWORK MANAGER");
    ui_row(1U, COLOR_LIGHT_CYAN, COLOR_BLACK,
        " LIVE CONNECTION  |  Manage your address, test connectivity, DNS, and HTTP");
    ui_row(2U, COLOR_DARK_GRAY, COLOR_BLACK,
        " HTTP is unencrypted; package downloads should verify content before installation.");
    ui_row(3U, COLOR_YELLOW, COLOR_BLACK, " CONNECTION");

    ui_draw_connection_rows(state, NETWORK_UI_DIRTY_ALL);

    ui_row(9U, COLOR_WHITE, COLOR_DARK_GRAY, " QUICK ACTIONS   |   Select one, then press Enter");
    for (uint32_t index = 0U; index < NETWORK_UI_MENU_COUNT; index++) {
        ui_draw_menu_selection(state, index);
    }

    if (state->rows > 23U) {
        ui_fill_row(20U, COLOR_DARK_GRAY, COLOR_BLACK);
        console_set_cursor(2, 20);
        if (tcp_get_state() == TCP_STATE_ESTABLISHED || tcp_get_state() == TCP_STATE_CLOSE_WAIT) {
            console_write("TCP peer ready. Send text, read responses, or close the session from Quick Actions.");
        } else {
            console_write("Tip: QEMU user networking uses 10.0.2.2 as its gateway.");
        }
    }
    ui_draw_status(state);
    ui_row(state->rows - 2U, COLOR_LIGHT_GRAY, COLOR_BLACK,
        " Up/Down or 1-9,0 to choose   Enter to open   G/P/D/T/S/R/C/U/N/H shortcuts");
    ui_row(state->rows - 1U, COLOR_BLACK, COLOR_LIGHT_GRAY,
        " Esc / Q  Exit                                      PrismOS Network Manager ");
    for (uint32_t row = NETWORK_UI_MENU_FIRST_ROW + NETWORK_UI_MENU_COUNT + 1U;
        row + 3U < state->rows; row++) {
        ui_clear_row(row);
    }
}

static void ui_draw_modal(const NetworkUiState* state) {
    uint32_t box_width = state->columns > 78U ? 74U : state->columns - 4U;
    uint32_t box_height = state->modal == NETWORK_MODAL_TCP_READ
        || state->modal == NETWORK_MODAL_HTTP_RESULT ? 13U : 9U;
    uint32_t x = (state->columns - box_width) / 2U;
    uint32_t y = (state->rows - box_height) / 2U;
    const char* title = "Network action";
    const char* instruction = "";
    char prompt[NETWORK_UI_INPUT_CAPACITY + 2U];

    switch (state->modal) {
        case NETWORK_MODAL_PING_ADDRESS:
            title = "PING AN IPV4 ADDRESS";
            instruction = "Enter the destination, for example 1.1.1.1";
            break;
        case NETWORK_MODAL_TCP_ADDRESS:
            title = "CONNECT TO A TCP SERVICE";
            instruction = "Enter the server IPv4 address";
            break;
        case NETWORK_MODAL_TCP_PORT:
            title = "REMOTE TCP PORT";
            instruction = "Enter a port from 1 to 65535";
            break;
        case NETWORK_MODAL_TCP_SEND:
            title = "SEND TCP TEXT";
            instruction = "Enter text to send (maximum 95 characters)";
            break;
        case NETWORK_MODAL_TCP_READ:
            title = "TCP RESPONSE";
            instruction = "Received data (non-printable bytes appear as dots)";
            break;
        case NETWORK_MODAL_DNS_HOSTNAME:
            title = "DNS LOOKUP";
            instruction = "Enter a hostname to resolve with the DHCP DNS server";
            break;
        case NETWORK_MODAL_DNS_RESULT:
            title = "DNS LOOKUP RESULT";
            instruction = "Resolved IPv4 address";
            break;
        case NETWORK_MODAL_HTTP_HOSTNAME:
            title = "HTTP GET - SERVER";
            instruction = "Enter a hostname or dotted IPv4 address";
            break;
        case NETWORK_MODAL_HTTP_PATH:
            title = "HTTP GET - PATH";
            instruction = "Enter a path beginning with /, for example /packages/index";
            break;
        case NETWORK_MODAL_HTTP_PORT:
            title = "HTTP GET - PORT";
            instruction = "Enter the server TCP port (80 is the default)";
            break;
        case NETWORK_MODAL_HTTP_RESULT:
            title = "HTTP RESPONSE PREVIEW";
            instruction = "Plain HTTP response body (non-printable bytes appear as dots)";
            break;
        default:
            break;
    }

    for (uint32_t row = 0U; row < box_height; row++) {
        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLUE);
        console_set_cursor((int)x, (int)(y + row));
        for (uint32_t column = 0U; column < box_width; column++) {
            console_write_char(' ');
        }
    }
    console_set_color(COLOR_WHITE, COLOR_BLUE);
    console_set_cursor((int)(x + 2U), (int)(y + 1U));
    ui_write_clipped(title, box_width - 4U);
    console_set_color(COLOR_LIGHT_CYAN, COLOR_BLUE);
    console_set_cursor((int)(x + 2U), (int)(y + 2U));
    ui_write_clipped(instruction, box_width - 4U);

    if (state->modal == NETWORK_MODAL_TCP_READ || state->modal == NETWORK_MODAL_DNS_RESULT
        || state->modal == NETWORK_MODAL_HTTP_RESULT) {
        if (state->modal == NETWORK_MODAL_HTTP_RESULT) {
            console_set_color(COLOR_YELLOW, COLOR_BLUE);
            console_set_cursor((int)(x + 2U), (int)(y + 3U));
            console_write("HTTP status ");
            console_write_uint(state->http_status_code);
            console_set_cursor((int)(x + 2U), (int)(y + 4U));
            console_write("Content-Type: ");
            ui_write_clipped(state->http_content_type[0] != '\0'
                ? state->http_content_type : "unknown", box_width - 18U);
        }
        uint32_t preview_rows = state->modal == NETWORK_MODAL_DNS_RESULT ? 1U : NETWORK_UI_READ_ROWS;
        for (uint32_t line = 0U; line < preview_rows; line++) {
            uint32_t start = line * NETWORK_UI_READ_COLUMNS;
            console_set_color(COLOR_WHITE, COLOR_BLUE);
            console_set_cursor((int)(x + 2U), (int)(y + (state->modal == NETWORK_MODAL_HTTP_RESULT ? 5U : 4U) + line));
            if (start >= state->received_length) {
                continue;
            }
            for (uint32_t index = start; index < state->received_length
                && index < start + NETWORK_UI_READ_COLUMNS; index++) {
                uint8_t character = state->received[index];
                console_write_char(character >= 32U && character <= 126U
                    ? (char)character : '.');
            }
        }
        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLUE);
        console_set_cursor((int)(x + 2U), (int)(y + box_height - 2U));
        ui_write_clipped("Esc / Enter returns to the dashboard", box_width - 4U);
    } else {
        uint32_t index = 0U;
        while (state->input[index] != '\0' && index + 1U < sizeof(prompt)) {
            prompt[index] = state->input[index];
            index++;
        }
        prompt[index++] = '_';
        prompt[index] = '\0';
        console_set_color(COLOR_YELLOW, COLOR_BLUE);
        console_set_cursor((int)(x + 2U), (int)(y + 5U));
        ui_write_clipped(prompt, box_width - 4U);
        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLUE);
        console_set_cursor((int)(x + 2U), (int)(y + box_height - 2U));
        ui_write_clipped("Enter accepts     Backspace edits     Esc cancels", box_width - 4U);
    }
}

static void ui_draw(NetworkUiState* state) {
    ui_draw_dashboard(state);
    if (state->modal != NETWORK_MODAL_NONE) {
        ui_draw_modal(state);
    }
    state->last_draw_ms = system_uptime_ms();
    state->status_dirty = 0U;
    state->modal_dirty = 0U;
    state->full_draw_generation++;
}

static int ui_parse_ipv4(const char* text, uint8_t address[4]) {
    const char* cursor = text;
    for (uint32_t octet = 0U; octet < 4U; octet++) {
        uint32_t value = 0U;
        uint32_t digits = 0U;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10U + (uint32_t)(*cursor - '0');
            if (value > 255U) return -1;
            cursor++;
            digits++;
        }
        if (digits == 0U) return -1;
        address[octet] = (uint8_t)value;
        if (octet < 3U) {
            if (*cursor != '.') return -1;
            cursor++;
        } else if (*cursor != '\0') {
            return -1;
        }
    }
    return 0;
}

static int ui_parse_port(const char* text, uint16_t* port) {
    uint32_t value = 0U;
    if (*text == '\0') return -1;
    while (*text != '\0') {
        if (*text < '0' || *text > '9') return -1;
        {
            uint32_t digit = (uint32_t)(*text - '0');
            if (value > (65535U - digit) / 10U) return -1;
            value = value * 10U + digit;
        }
        text++;
    }
    if (value == 0U) return -1;
    *port = (uint16_t)value;
    return 0;
}

static void ui_open_modal(NetworkUiState* state, NetworkModal modal) {
    state->modal = modal;
    state->input_length = 0U;
    state->input[0] = '\0';
    state->modal_dirty = 1U;
}

static void ui_run_ping(NetworkUiState* state, const uint8_t address[4]) {
    uint32_t round_trip_ms;
    ui_status(state, "Sending ICMP Echo Request ...", COLOR_YELLOW);
    ui_draw(state);
    if (icmp_ping(address, 2500U, &round_trip_ms) == 0) {
        ui_status(state, "Ping reply received successfully.", COLOR_LIGHT_GREEN);
    } else {
        ui_status(state, "No reply. Check the address, gateway, link, and firewall.", COLOR_LIGHT_RED);
    }
}

static void ui_append_uint8(uint8_t* output, uint16_t* length, uint8_t value) {
    uint8_t digits[3];
    uint32_t count = 0U;
    do {
        digits[count++] = (uint8_t)('0' + value % 10U);
        value = (uint8_t)(value / 10U);
    } while (value != 0U);
    while (count > 0U) output[(*length)++] = digits[--count];
}

static void ui_run_dns_lookup(NetworkUiState* state, const char* hostname) {
    uint16_t length = 0U;
    if (dns_resolve_ipv4(hostname, 5000U, state->target_address) != 0) {
        state->modal = NETWORK_MODAL_NONE;
        ui_status(state, "DNS lookup failed. Check DHCP DNS settings and the hostname.", COLOR_LIGHT_RED);
        return;
    }
    state->received[length++] = 'I';
    state->received[length++] = 'P';
    state->received[length++] = 'v';
    state->received[length++] = '4';
    state->received[length++] = ':';
    state->received[length++] = ' ';
    for (uint32_t octet = 0U; octet < 4U; octet++) {
        if (octet != 0U) state->received[length++] = '.';
        ui_append_uint8(state->received, &length, state->target_address[octet]);
    }
    state->received_length = length;
    state->modal = NETWORK_MODAL_DNS_RESULT;
    state->modal_dirty = 1U;
    ui_status(state, "Hostname resolved successfully.", COLOR_LIGHT_GREEN);
}

static void ui_run_http_get(NetworkUiState* state, uint16_t port) {
    http_response_t response;
    response.status_code = 0U;
    response.body_length = 0U;
    response.body = state->received;
    response.body_capacity = sizeof(state->received);
    response.content_type[0] = '\0';
    state->modal = NETWORK_MODAL_NONE;
    ui_status(state, "Connecting and fetching the HTTP response ...", COLOR_YELLOW);
    ui_draw(state);
    if (http_get(state->target_hostname, state->target_path, port, 10000U, &response) != 0) {
        ui_status(state, "HTTP request failed or response exceeded the 384-byte preview limit.", COLOR_LIGHT_RED);
        return;
    }
    state->http_status_code = response.status_code;
    ui_copy(state->http_content_type, sizeof(state->http_content_type), response.content_type);
    state->received_length = (uint16_t)response.body_length;
    state->modal = NETWORK_MODAL_HTTP_RESULT;
    state->modal_dirty = 1U;
    ui_status(state, "HTTP response received. Preview is limited to 384 body bytes.", COLOR_LIGHT_GREEN);
}

static void ui_execute_action(NetworkUiState* state, uint32_t action) {
    ipv4_configuration_t configuration;

    switch (action) {
        case 0U:
            if (ipv4_get_configuration(&configuration) != 0
                || (configuration.gateway[0] == 0U && configuration.gateway[1] == 0U
                    && configuration.gateway[2] == 0U && configuration.gateway[3] == 0U)) {
                ui_status(state, "No gateway is configured. Renew your DHCP lease first.", COLOR_LIGHT_RED);
            } else {
                ui_run_ping(state, configuration.gateway);
            }
            break;
        case 1U:
            ui_open_modal(state, NETWORK_MODAL_PING_ADDRESS);
            ui_status(state, "Enter an IPv4 destination to ping.", COLOR_LIGHT_CYAN);
            break;
        case 2U:
            if (dhcp_start() == 0) {
                ui_status(state, "DHCP discovery sent. The dashboard will update when a lease arrives.", COLOR_LIGHT_GREEN);
            } else {
                ui_status(state, "DHCP could not start. Check that an adapter is available and linked.", COLOR_LIGHT_RED);
            }
            break;
        case 3U:
            ui_open_modal(state, NETWORK_MODAL_TCP_ADDRESS);
            ui_status(state, "Enter the remote address to begin a TCP connection.", COLOR_LIGHT_CYAN);
            break;
        case 4U:
            if (tcp_get_state() != TCP_STATE_ESTABLISHED) {
                ui_status(state, "Connect to a TCP service before sending data.", COLOR_LIGHT_RED);
            } else {
                ui_open_modal(state, NETWORK_MODAL_TCP_SEND);
                ui_status(state, "Type the text to send to the connected peer.", COLOR_LIGHT_CYAN);
            }
            break;
        case 5U:
            if (tcp_read(state->received, sizeof(state->received), &state->received_length) != 0) {
                ui_status(state, "No TCP response is waiting to be read.", COLOR_YELLOW);
            } else {
                state->modal = NETWORK_MODAL_TCP_READ;
                state->modal_dirty = 1U;
                ui_status(state, "TCP data received from the remote peer.", COLOR_LIGHT_GREEN);
            }
            break;
        case 6U:
            if (tcp_close(3000U) == 0) {
                ui_status(state, "TCP session closed.", COLOR_LIGHT_GREEN);
            } else {
                ui_status(state, "No active TCP session to close.", COLOR_YELLOW);
            }
            break;
        case 8U:
            ui_open_modal(state, NETWORK_MODAL_DNS_HOSTNAME);
            ui_status(state, "Enter a hostname to resolve through DHCP-provided DNS.", COLOR_LIGHT_CYAN);
            break;
        case 9U:
            ui_open_modal(state, NETWORK_MODAL_HTTP_HOSTNAME);
            ui_status(state, "Enter an HTTP server hostname or IPv4 address.", COLOR_LIGHT_CYAN);
            break;
        default:
            network_poll();
            ui_status(state, "Dashboard refreshed.", COLOR_LIGHT_CYAN);
            break;
    }
}

static void ui_handle_modal_event(NetworkUiState* state, KeyEvent event) {
    if (event.type == KEY_EVENT_CHARACTER && event.character == 27) {
        state->modal = NETWORK_MODAL_NONE;
        return;
    }
    if (state->modal == NETWORK_MODAL_TCP_READ || state->modal == NETWORK_MODAL_DNS_RESULT
        || state->modal == NETWORK_MODAL_HTTP_RESULT) {
        if (event.type == KEY_EVENT_ENTER || event.type == KEY_EVENT_CHARACTER) {
            state->modal = NETWORK_MODAL_NONE;
        }
        return;
    }
    if (event.type == KEY_EVENT_BACKSPACE) {
        if (state->input_length > 0U) {
            state->input[--state->input_length] = '\0';
        }
        state->modal_dirty = state->modal != NETWORK_MODAL_NONE;
        return;
    }
    if (event.type == KEY_EVENT_ENTER) {
        if (state->modal == NETWORK_MODAL_PING_ADDRESS) {
            uint8_t address[4];
            if (ui_parse_ipv4(state->input, address) != 0) {
                ui_status(state, "Enter a valid dotted IPv4 address, e.g. 1.1.1.1.", COLOR_LIGHT_RED);
                return;
            }
            state->modal = NETWORK_MODAL_NONE;
            ui_run_ping(state, address);
        } else if (state->modal == NETWORK_MODAL_TCP_ADDRESS) {
            if (ui_parse_ipv4(state->input, state->target_address) != 0) {
                ui_status(state, "Enter a valid dotted IPv4 address.", COLOR_LIGHT_RED);
                return;
            }
            ui_open_modal(state, NETWORK_MODAL_TCP_PORT);
            ui_status(state, "Enter the remote TCP port, such as 80 or 443.", COLOR_LIGHT_CYAN);
        } else if (state->modal == NETWORK_MODAL_TCP_PORT) {
            if (ui_parse_port(state->input, &state->target_port) != 0) {
                ui_status(state, "Port must be a whole number from 1 to 65535.", COLOR_LIGHT_RED);
                return;
            }
            state->modal = NETWORK_MODAL_NONE;
            ui_status(state, "Connecting ...", COLOR_YELLOW);
            ui_draw(state);
            if (tcp_connect(state->target_address, state->target_port, 5000U) == 0) {
                ui_status(state, "TCP connection established. Choose Send or Read.", COLOR_LIGHT_GREEN);
            } else {
                ui_status(state, "Connection failed. Verify the host, port, and firewall.", COLOR_LIGHT_RED);
            }
        } else if (state->modal == NETWORK_MODAL_TCP_SEND) {
            uint32_t length = state->input_length;
            state->modal = NETWORK_MODAL_NONE;
            if (length == 0U) {
                ui_status(state, "Enter some text before sending.", COLOR_YELLOW);
            } else {
                ui_status(state, "Sending data to the TCP peer ...", COLOR_YELLOW);
                ui_draw(state);
                if (tcp_send((const uint8_t*)state->input, (uint16_t)length, 3000U) == 0) {
                    ui_status(state, "Data sent and acknowledged by the TCP peer.", COLOR_LIGHT_GREEN);
                } else {
                    ui_status(state, "Send failed or timed out waiting for acknowledgement.", COLOR_LIGHT_RED);
                }
            }
        } else if (state->modal == NETWORK_MODAL_DNS_HOSTNAME) {
            if (state->input_length == 0U) {
                ui_status(state, "Enter a hostname before starting DNS lookup.", COLOR_LIGHT_RED);
                return;
            }
            state->modal = NETWORK_MODAL_NONE;
            ui_status(state, "Looking up hostname ...", COLOR_YELLOW);
            ui_draw(state);
            ui_run_dns_lookup(state, state->input);
        } else if (state->modal == NETWORK_MODAL_HTTP_HOSTNAME) {
            if (state->input_length == 0U) {
                ui_status(state, "Enter an HTTP server hostname or IPv4 address.", COLOR_LIGHT_RED);
                return;
            }
            ui_copy(state->target_hostname, sizeof(state->target_hostname), state->input);
            ui_open_modal(state, NETWORK_MODAL_HTTP_PATH);
            ui_status(state, "Enter the URL path to request.", COLOR_LIGHT_CYAN);
        } else if (state->modal == NETWORK_MODAL_HTTP_PATH) {
            if (state->input_length == 0U || state->input[0] != '/') {
                ui_status(state, "HTTP path must begin with /, for example /index.html.", COLOR_LIGHT_RED);
                return;
            }
            ui_copy(state->target_path, sizeof(state->target_path), state->input);
            ui_open_modal(state, NETWORK_MODAL_HTTP_PORT);
            state->input[0] = '8';
            state->input[1] = '0';
            state->input[2] = '\0';
            state->input_length = 2U;
            ui_status(state, "Use port 80 or enter the server's HTTP port.", COLOR_LIGHT_CYAN);
        } else if (state->modal == NETWORK_MODAL_HTTP_PORT) {
            uint16_t port;
            if (ui_parse_port(state->input, &port) != 0) {
                ui_status(state, "Port must be a whole number from 1 to 65535.", COLOR_LIGHT_RED);
                return;
            }
            ui_run_http_get(state, port);
        }
        state->modal_dirty = 1U;
        return;
    }
    if (event.type == KEY_EVENT_CHARACTER && state->input_length + 1U < sizeof(state->input)) {
        char character = event.character;
        int accepted = 0;
        if (state->modal == NETWORK_MODAL_PING_ADDRESS
            || state->modal == NETWORK_MODAL_TCP_ADDRESS) {
            accepted = (character >= '0' && character <= '9') || character == '.';
        } else if (state->modal == NETWORK_MODAL_DNS_HOSTNAME
            || state->modal == NETWORK_MODAL_HTTP_HOSTNAME) {
            accepted = (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '.' || character == '-';
        } else if (state->modal == NETWORK_MODAL_TCP_PORT) {
            accepted = character >= '0' && character <= '9';
        } else if (state->modal == NETWORK_MODAL_HTTP_PORT) {
            accepted = character >= '0' && character <= '9';
        } else if (state->modal == NETWORK_MODAL_HTTP_PATH) {
            accepted = character >= 33 && character <= 126;
        } else if (state->modal == NETWORK_MODAL_TCP_SEND) {
            accepted = character >= 32 && character <= 126;
        }
        if (accepted) {
            state->input[state->input_length++] = character;
            state->input[state->input_length] = '\0';
            state->modal_dirty = 1U;
        }
    }
}

int network_manager_app_run(void) {
    NetworkUiState state = {0};
    state.columns = console_get_framebuffer_width() / 8U;
    state.rows = console_get_framebuffer_height() / 16U;
    state.status_color = COLOR_LIGHT_CYAN;
    state.running = 1;
    ui_status(&state, "Ready. Choose a network action or watch live connection status.", COLOR_LIGHT_CYAN);

    if (state.columns < NETWORK_UI_MIN_COLUMNS || state.rows < NETWORK_UI_MIN_ROWS) {
        console_writeln("Network manager needs a display at least 76 columns by 23 rows.");
        return -1;
    }

    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    console_clear();
    ui_draw(&state);
    while (state.running) {
        KeyEvent event;
        uint32_t now;
        network_poll();
        event = keyboard_poll_event();
        now = system_uptime_ms();
        if ((uint32_t)(now - state.last_draw_ms) >= 250U) {
            if (state.modal == NETWORK_MODAL_NONE) {
                uint32_t changed_rows = ui_connection_change_mask(&state);
                if (changed_rows != 0U) {
                    ui_draw_connection_rows(&state, changed_rows);
                }
            }
            state.last_draw_ms = now;
        }
        if (state.status_dirty) {
            ui_draw_status(&state);
            state.status_dirty = 0U;
        }
        if (state.modal_dirty && state.modal != NETWORK_MODAL_NONE) {
            ui_draw_modal(&state);
            state.modal_dirty = 0U;
        }
        if (event.type == KEY_EVENT_NONE) {
            __asm__ volatile ("hlt");
            continue;
        }
        if (state.modal != NETWORK_MODAL_NONE) {
            NetworkModal previous_modal = state.modal;
            uint32_t previous_draw_generation = state.full_draw_generation;
            ui_handle_modal_event(&state, event);
            if (previous_modal != NETWORK_MODAL_NONE && state.modal == NETWORK_MODAL_NONE) {
                if (state.full_draw_generation == previous_draw_generation) {
                    ui_draw(&state);
                } else if (state.status_dirty) {
                    ui_draw_status(&state);
                    state.status_dirty = 0U;
                }
            } else {
                if (state.status_dirty) {
                    ui_draw_status(&state);
                    state.status_dirty = 0U;
                }
                if (state.modal_dirty && state.modal != NETWORK_MODAL_NONE) {
                    ui_draw_modal(&state);
                    state.modal_dirty = 0U;
                }
            }
            continue;
        }
        if (event.type == KEY_EVENT_UP) {
            uint32_t previous_selection = state.selected_action;
            state.selected_action = state.selected_action == 0U
                ? NETWORK_UI_MENU_COUNT - 1U : state.selected_action - 1U;
            ui_draw_menu_selection(&state, previous_selection);
            ui_draw_menu_selection(&state, state.selected_action);
        } else if (event.type == KEY_EVENT_DOWN) {
            uint32_t previous_selection = state.selected_action;
            state.selected_action = (state.selected_action + 1U) % NETWORK_UI_MENU_COUNT;
            ui_draw_menu_selection(&state, previous_selection);
            ui_draw_menu_selection(&state, state.selected_action);
        } else if (event.type == KEY_EVENT_ENTER) {
            ui_execute_action(&state, state.selected_action);
        } else if (event.type == KEY_EVENT_CHARACTER) {
            char character = event.character;
            if (character >= 'A' && character <= 'Z') {
                character = (char)(character - 'A' + 'a');
            }
            if (character == 27 || character == 'q') {
                state.running = 0;
            } else if (character >= '1' && character <= '8') {
                ui_execute_action(&state, (uint32_t)(character - '1'));
            } else if (character == '9') {
                ui_execute_action(&state, 8U);
            } else if (character == '0') {
                ui_execute_action(&state, 9U);
            } else if (character == 'g') {
                ui_execute_action(&state, 0U);
            } else if (character == 'p') {
                ui_execute_action(&state, 1U);
            } else if (character == 'd') {
                ui_execute_action(&state, 2U);
            } else if (character == 't') {
                ui_execute_action(&state, 3U);
            } else if (character == 's') {
                ui_execute_action(&state, 4U);
            } else if (character == 'r') {
                ui_execute_action(&state, 5U);
            } else if (character == 'c') {
                ui_execute_action(&state, 6U);
            } else if (character == 'u') {
                ui_execute_action(&state, 7U);
            } else if (character == 'n') {
                ui_execute_action(&state, 8U);
            } else if (character == 'h') {
                ui_execute_action(&state, 9U);
            }
        }
    }

    console_set_cursor_visible(1);
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    console_clear();
    console_set_cursor(0, 0);
    return 0;
}
