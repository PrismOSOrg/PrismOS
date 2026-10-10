#include "apps/package_manager.h"

#include <stdint.h>

#include "apps/app_format.h"
#include "debug/log.h"
#include "display/console.h"
#include "filesystem/vfs.h"
#include "net/http.h"
#include "util/sha256.h"

#define CATALOG_CAPACITY 6144U
#define MANIFEST_CAPACITY 2048U
#define PACKAGE_CAPACITY 65536U
#define CHUNK_CAPACITY 4096U
#define REPOSITORY_HOST "10.0.2.2"
#define REPOSITORY_PORT 8080U
#define REQUEST_TIMEOUT_MS 15000U
#define INSTALLED_DB "/PKGDB.TXT"
#define FAVORITES_DB "/PKGFAV.TXT"
#define PACKAGES_DIR "/PKGS"
#define INSTALLED_DB_CAPACITY 4096U
#define FAVORITES_DB_CAPACITY 2048U

typedef struct {
    char id[PACKAGE_MANAGER_ID_CAPACITY];
    char version[PACKAGE_MANAGER_VERSION_CAPACITY];
    char path[PACKAGE_MANAGER_PATH_CAPACITY];
} InstalledRecord;

typedef struct {
    char id[PACKAGE_MANAGER_ID_CAPACITY];
    char version[PACKAGE_MANAGER_VERSION_CAPACITY];
} FavoriteRecord;

typedef struct {
    char id[PACKAGE_MANAGER_ID_CAPACITY];
    char version[PACKAGE_MANAGER_VERSION_CAPACITY];
    char sha256[PACKAGE_MANAGER_HASH_CAPACITY];
    char description[121];
    uint32_t size;
    uint32_t chunk_size;
} PackageManifest;

static uint8_t catalog_buffer[CATALOG_CAPACITY + 1U];
static uint8_t manifest_buffer[MANIFEST_CAPACITY + 1U];
static uint8_t package_buffer[PACKAGE_CAPACITY];
static uint8_t chunk_buffer[CHUNK_CAPACITY];
static char installed_db_buffer[INSTALLED_DB_CAPACITY];
static char favorites_db_buffer[FAVORITES_DB_CAPACITY];
static package_manager_entry_t package_entries[PACKAGE_MANAGER_MAX_PACKAGES];
static InstalledRecord installed_records[PACKAGE_MANAGER_MAX_PACKAGES];
static FavoriteRecord favorite_records[PACKAGE_MANAGER_MAX_PACKAGES];
static uint32_t package_count;
static uint32_t installed_count;
static uint32_t favorite_count;

static uint32_t text_length(const char* text) {
    uint32_t length = 0U;
    while (text[length] != '\0') length++;
    return length;
}

static void text_copy(char* destination, uint32_t capacity, const char* source) {
    uint32_t index = 0U;
    if (capacity == 0U) return;
    while (source[index] != '\0' && index + 1U < capacity) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

static int text_equal(const char* left, const char* right) {
    while (*left != '\0' && *left == *right) { left++; right++; }
    return *left == '\0' && *right == '\0';
}

static int bytes_equal(const uint8_t* bytes, uint32_t length, const char* text) {
    if (text_length(text) != length) return 0;
    for (uint32_t index = 0U; index < length; index++) {
        if (bytes[index] != (uint8_t)text[index]) return 0;
    }
    return 1;
}

static int segment_valid(const uint8_t* text, uint32_t length) {
    if (length == 0U || length > 32U
        || !((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z')
            || (text[0] >= '0' && text[0] <= '9'))) return 0;
    for (uint32_t index = 1U; index < length; index++) {
        uint8_t c = text[index];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int parse_decimal(const uint8_t* text, uint32_t length, uint32_t* out_value) {
    uint32_t value = 0U;
    if (length == 0U || out_value == 0 || (length > 1U && text[0] == '0')) return -1;
    for (uint32_t index = 0U; index < length; index++) {
        uint32_t digit;
        if (text[index] < '0' || text[index] > '9') return -1;
        digit = (uint32_t)(text[index] - '0');
        if (value > (0xFFFFFFFFU - digit) / 10U) return -1;
        value = value * 10U + digit;
    }
    *out_value = value;
    return 0;
}

static int sha_text_valid(const uint8_t* text, uint32_t length) {
    if (length != 64U) return 0;
    for (uint32_t index = 0U; index < length; index++) {
        if (!((text[index] >= '0' && text[index] <= '9')
            || (text[index] >= 'a' && text[index] <= 'f'))) return 0;
    }
    return 1;
}

static int content_type_is(const char* actual, const char* expected) {
    uint32_t index = 0U;
    while (expected[index] != '\0') {
        char c = actual[index];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != expected[index]) return 0;
        index++;
    }
    return actual[index] == '\0' || actual[index] == ';';
}

static int make_endpoint(char* output, uint32_t capacity, const char* prefix,
    const char* id, const char* separator, const char* version, const char* suffix) {
    const char* parts[5] = {prefix, id, separator, version, suffix};
    uint32_t length = 0U;
    for (uint32_t part = 0U; part < 5U; part++) {
        uint32_t part_length = text_length(parts[part]);
        if (part_length >= capacity - length) return -1;
        for (uint32_t index = 0U; index < part_length; index++) output[length++] = parts[part][index];
    }
    output[length] = '\0';
    return 0;
}

static int http_fetch(const char* path, uint8_t* body, uint32_t capacity,
    uint32_t* out_length, const char* content_type) {
    http_response_t response = {0};
    response.body = body;
    response.body_capacity = capacity;
    int result = http_get(REPOSITORY_HOST, path, REPOSITORY_PORT,
        REQUEST_TIMEOUT_MS, &response);
    if (result != 0) DEBUG_LOG("pkg: HTTP request failed before a valid response was parsed");
    if (result == -2) return PACKAGE_MANAGER_ERROR_DNS;
    if (result == -3) return PACKAGE_MANAGER_ERROR_CONNECT;
    if (result == -5) return PACKAGE_MANAGER_ERROR_TIMEOUT;
    if (result != 0) return PACKAGE_MANAGER_ERROR_NETWORK;
    if (response.status_code != 200U || !content_type_is(response.content_type, content_type))
        return PACKAGE_MANAGER_ERROR_PROTOCOL;
    if (response.body_length > capacity) return PACKAGE_MANAGER_ERROR_LIMIT;
    *out_length = response.body_length;
    return PACKAGE_MANAGER_OK;
}

static int parse_catalog_row(const uint8_t* row, uint32_t length,
    package_manager_entry_t* entry) {
    uint32_t starts[5], lengths[5], count = 1U, start = 0U, size;
    for (uint32_t index = 0U; index <= length; index++) {
        if (index == length || row[index] == '\t') {
            if (count > 5U) return -1;
            starts[count - 1U] = start;
            lengths[count - 1U] = index - start;
            if (index != length) { count++; start = index + 1U; }
        } else if (row[index] < 0x20U || row[index] > 0x7EU) return -1;
    }
    if (count != 5U || !segment_valid(&row[starts[0]], lengths[0])
        || !segment_valid(&row[starts[1]], lengths[1])
        || parse_decimal(&row[starts[2]], lengths[2], &size) != 0 || size == 0U
        || !sha_text_valid(&row[starts[3]], lengths[3])) return -1;

    char expected_path[128];
    char id[PACKAGE_MANAGER_ID_CAPACITY], version[PACKAGE_MANAGER_VERSION_CAPACITY];
    for (uint32_t i = 0U; i < lengths[0]; i++) id[i] = (char)row[starts[0] + i];
    id[lengths[0]] = '\0';
    for (uint32_t i = 0U; i < lengths[1]; i++) version[i] = (char)row[starts[1] + i];
    version[lengths[1]] = '\0';
    if (make_endpoint(expected_path, sizeof(expected_path), "/api/v1/packages/", id,
            "/", version, "/manifest") != 0
        || !bytes_equal(&row[starts[4]], lengths[4], expected_path)) return -1;

    text_copy(entry->id, sizeof(entry->id), id);
    text_copy(entry->version, sizeof(entry->version), version);
    for (uint32_t i = 0U; i < 64U; i++) entry->sha256[i] = (char)row[starts[3] + i];
    entry->sha256[64] = '\0';
    entry->size = size;
    entry->installed = 0U;
    entry->favorite = 0U;
    entry->installed_path[0] = '\0';
    return 0;
}

static int path_is_installed_file(const char* path) {
    static const char prefix[] = "/PKGS/P";
    uint32_t prefix_length = sizeof(prefix) - 1U;
    if (path == 0 || text_length(path) != prefix_length + 7U + 4U) return 0;
    for (uint32_t i = 0U; i < prefix_length; i++) if (path[i] != prefix[i]) return 0;
    for (uint32_t i = 0U; i < 7U; i++) if (path[prefix_length + i] < '0' || path[prefix_length + i] > '9') return 0;
    return path[prefix_length + 7U] == '.' && path[prefix_length + 8U] == 'A'
        && path[prefix_length + 9U] == 'P' && path[prefix_length + 10U] == 'P';
}

static int parse_local_records(void) {
    static const char installed_header[] = "PRISMPKG-INSTALLED/1\n";
    static const char favorites_header[] = "PRISMPKG-FAVORITES/1\n";
    uint32_t size = 0U;
    installed_count = 0U;
    favorite_count = 0U;

    int is_directory = 0;
    if (vfs_path_is_dir(PACKAGES_DIR, &is_directory) != 0) {
        if (vfs_mkdir(PACKAGES_DIR) != 0) return -1;
    } else if (!is_directory) return -1;
    if (vfs_touch(INSTALLED_DB) != 0 || vfs_touch(FAVORITES_DB) != 0) return -1;

    if (vfs_read_file(INSTALLED_DB, installed_db_buffer, sizeof(installed_db_buffer) - 1U, &size) != 0)
        return -1;
    installed_db_buffer[size] = '\0';
    if (size == 0U) {
        size = sizeof(installed_header) - 1U;
        if (vfs_write_file(INSTALLED_DB, installed_header, size, 0) != 0) return -1;
        text_copy(installed_db_buffer, sizeof(installed_db_buffer), installed_header);
    } else if (size < sizeof(installed_header) - 1U
        || !bytes_equal((const uint8_t*)installed_db_buffer, sizeof(installed_header) - 1U, installed_header)) return -1;

    for (uint32_t offset = sizeof(installed_header) - 1U; offset < size;) {
        uint32_t end = offset, tabs[2], tab_count = 0U;
        while (end < size && installed_db_buffer[end] != '\n') {
            if (installed_db_buffer[end] == '\t' && tab_count < 2U) tabs[tab_count++] = end;
            end++;
        }
        if (tab_count != 2U || installed_count >= PACKAGE_MANAGER_MAX_PACKAGES
            || !segment_valid((uint8_t*)&installed_db_buffer[offset], tabs[0] - offset)
            || !segment_valid((uint8_t*)&installed_db_buffer[tabs[0] + 1U], tabs[1] - tabs[0] - 1U)) return -1;
        installed_db_buffer[tabs[0]] = '\0';
        installed_db_buffer[tabs[1]] = '\0';
        InstalledRecord* record = &installed_records[installed_count++];
        text_copy(record->id, sizeof(record->id), &installed_db_buffer[offset]);
        text_copy(record->version, sizeof(record->version), &installed_db_buffer[tabs[0] + 1U]);
        text_copy(record->path, sizeof(record->path), &installed_db_buffer[tabs[1] + 1U]);
        if (!path_is_installed_file(record->path)) return -1;
        offset = end < size ? end + 1U : size;
    }

    size = 0U;
    if (vfs_read_file(FAVORITES_DB, favorites_db_buffer, sizeof(favorites_db_buffer) - 1U, &size) != 0)
        return -1;
    favorites_db_buffer[size] = '\0';
    if (size == 0U) {
        size = sizeof(favorites_header) - 1U;
        if (vfs_write_file(FAVORITES_DB, favorites_header, size, 0) != 0) return -1;
        text_copy(favorites_db_buffer, sizeof(favorites_db_buffer), favorites_header);
    } else if (size < sizeof(favorites_header) - 1U
        || !bytes_equal((const uint8_t*)favorites_db_buffer, sizeof(favorites_header) - 1U, favorites_header)) return -1;

    for (uint32_t offset = sizeof(favorites_header) - 1U; offset < size;) {
        uint32_t end = offset, tab = 0U;
        while (end < size && favorites_db_buffer[end] != '\n') {
            if (favorites_db_buffer[end] == '\t') tab = end;
            end++;
        }
        if (tab <= offset || tab + 1U >= end || favorite_count >= PACKAGE_MANAGER_MAX_PACKAGES
            || !segment_valid((uint8_t*)&favorites_db_buffer[offset], tab - offset)
            || !segment_valid((uint8_t*)&favorites_db_buffer[tab + 1U], end - tab - 1U)) return -1;
        favorites_db_buffer[tab] = '\0';
        FavoriteRecord* record = &favorite_records[favorite_count++];
        text_copy(record->id, sizeof(record->id), &favorites_db_buffer[offset]);
        text_copy(record->version, sizeof(record->version), &favorites_db_buffer[tab + 1U]);
        offset = end < size ? end + 1U : size;
    }
    return 0;
}

static int save_installed_records(void) {
    static const char header[] = "PRISMPKG-INSTALLED/1\n";
    uint32_t length = sizeof(header) - 1U;
    for (uint32_t i = 0U; i < sizeof(header) - 1U; i++) installed_db_buffer[i] = header[i];
    for (uint32_t i = 0U; i < installed_count; i++) {
        const char* fields[] = {installed_records[i].id, "\t", installed_records[i].version,
            "\t", installed_records[i].path, "\n"};
        for (uint32_t f = 0U; f < 6U; f++) {
            uint32_t n = text_length(fields[f]);
            if (n >= sizeof(installed_db_buffer) - length) return -1;
            for (uint32_t j = 0U; j < n; j++) installed_db_buffer[length++] = fields[f][j];
        }
    }
    return vfs_write_file(INSTALLED_DB, installed_db_buffer, length, 0);
}

static int save_favorite_records(void) {
    static const char header[] = "PRISMPKG-FAVORITES/1\n";
    uint32_t length = sizeof(header) - 1U;
    for (uint32_t i = 0U; i < sizeof(header) - 1U; i++) favorites_db_buffer[i] = header[i];
    for (uint32_t i = 0U; i < favorite_count; i++) {
        const char* fields[] = {favorite_records[i].id, "\t", favorite_records[i].version, "\n"};
        for (uint32_t f = 0U; f < 4U; f++) {
            uint32_t n = text_length(fields[f]);
            if (n >= sizeof(favorites_db_buffer) - length) return -1;
            for (uint32_t j = 0U; j < n; j++) favorites_db_buffer[length++] = fields[f][j];
        }
    }
    return vfs_write_file(FAVORITES_DB, favorites_db_buffer, length, 0);
}

static int fetch_manifest(const package_manager_entry_t* entry, PackageManifest* manifest) {
    char path[128];
    uint32_t length = 0U;
    if (make_endpoint(path, sizeof(path), "/api/v1/packages/", entry->id,
            "/", entry->version, "/manifest") != 0) return PACKAGE_MANAGER_ERROR_PROTOCOL;
    int result = http_fetch(path, manifest_buffer, MANIFEST_CAPACITY, &length, "text/plain");
    if (result != PACKAGE_MANAGER_OK) return result;
    manifest_buffer[length] = '\0';

    static const char header[] = "PRISMPKG-MANIFEST/1\n";
    if (length < sizeof(header) - 1U
        || !bytes_equal(manifest_buffer, sizeof(header) - 1U, header)) return PACKAGE_MANAGER_ERROR_PROTOCOL;
    uint8_t have_id = 0U, have_version = 0U, have_size = 0U, have_hash = 0U;
    uint8_t have_chunks = 0U, have_path = 0U;
    for (uint32_t offset = sizeof(header) - 1U; offset < length;) {
        uint32_t end = offset, tab = offset;
        while (end < length && manifest_buffer[end] != '\n') {
            if (manifest_buffer[end] == '\t' && tab == offset) tab = end;
            if (manifest_buffer[end] == '\r' || manifest_buffer[end] == '\0') return PACKAGE_MANAGER_ERROR_PROTOCOL;
            end++;
        }
        if (tab == offset) return PACKAGE_MANAGER_ERROR_PROTOCOL;
        uint32_t key_length = tab - offset;
        const uint8_t* value = &manifest_buffer[tab + 1U];
        uint32_t value_length = end - tab - 1U;
        if (bytes_equal(&manifest_buffer[offset], key_length, "id")) {
            if (!bytes_equal(value, value_length, entry->id)) return PACKAGE_MANAGER_ERROR_PROTOCOL;
            have_id = 1U;
        } else if (bytes_equal(&manifest_buffer[offset], key_length, "version")) {
            if (!bytes_equal(value, value_length, entry->version)) return PACKAGE_MANAGER_ERROR_PROTOCOL;
            have_version = 1U;
        } else if (bytes_equal(&manifest_buffer[offset], key_length, "size")) {
            if (parse_decimal(value, value_length, &manifest->size) != 0) return PACKAGE_MANAGER_ERROR_PROTOCOL;
            have_size = 1U;
        } else if (bytes_equal(&manifest_buffer[offset], key_length, "sha256")) {
            if (!sha_text_valid(value, value_length)) return PACKAGE_MANAGER_ERROR_PROTOCOL;
            for (uint32_t i = 0U; i < 64U; i++) manifest->sha256[i] = (char)value[i];
            manifest->sha256[64] = '\0';
            have_hash = 1U;
        } else if (bytes_equal(&manifest_buffer[offset], key_length, "chunk-size")) {
            if (parse_decimal(value, value_length, &manifest->chunk_size) != 0
                || manifest->chunk_size == 0U || manifest->chunk_size > CHUNK_CAPACITY)
                return PACKAGE_MANAGER_ERROR_PROTOCOL;
            have_chunks = 1U;
        } else if (bytes_equal(&manifest_buffer[offset], key_length, "chunk-path")) {
            char expected[128];
            if (make_endpoint(expected, sizeof(expected), "/api/v1/packages/", entry->id,
                    "/", entry->version, "/chunk") != 0
                || !bytes_equal(value, value_length, expected)) return PACKAGE_MANAGER_ERROR_PROTOCOL;
            have_path = 1U;
        } else if (bytes_equal(&manifest_buffer[offset], key_length, "description")) {
            uint32_t copied = value_length < sizeof(manifest->description) - 1U
                ? value_length : sizeof(manifest->description) - 1U;
            for (uint32_t i = 0U; i < copied; i++)
                manifest->description[i] = value[i] >= 32U && value[i] <= 126U ? (char)value[i] : ' ';
            manifest->description[copied] = '\0';
        }
        offset = end < length ? end + 1U : length;
    }
    if (!have_id || !have_version || !have_size || !have_hash || !have_chunks || !have_path
        || manifest->size != entry->size || !text_equal(manifest->sha256, entry->sha256))
        return PACKAGE_MANAGER_ERROR_PROTOCOL;
    return PACKAGE_MANAGER_OK;
}

static int key_matches(const char* left_id, const char* left_version,
    const char* right_id, const char* right_version) {
    return text_equal(left_id, right_id) && text_equal(left_version, right_version);
}

static void annotate_entries(void) {
    for (uint32_t i = 0U; i < package_count; i++) {
        package_entries[i].installed = 0U;
        package_entries[i].favorite = 0U;
        package_entries[i].installed_path[0] = '\0';
        for (uint32_t j = 0U; j < installed_count; j++) {
            if (key_matches(package_entries[i].id, package_entries[i].version,
                    installed_records[j].id, installed_records[j].version)) {
                package_entries[i].installed = 1U;
                text_copy(package_entries[i].installed_path, sizeof(package_entries[i].installed_path),
                    installed_records[j].path);
                break;
            }
        }
        for (uint32_t j = 0U; j < favorite_count; j++) {
            if (key_matches(package_entries[i].id, package_entries[i].version,
                    favorite_records[j].id, favorite_records[j].version)) {
                package_entries[i].favorite = 1U;
                break;
            }
        }
    }
}

int package_manager_refresh(void) {
    uint32_t length = 0U;
    int result = http_fetch("/api/v1/catalog", catalog_buffer, CATALOG_CAPACITY,
        &length, "text/plain");
    if (result != PACKAGE_MANAGER_OK) return result;
    static const char header[] = "PRISMPKG-CATALOG/1\n";
    if (length < sizeof(header) - 1U
        || !bytes_equal(catalog_buffer, sizeof(header) - 1U, header)) return PACKAGE_MANAGER_ERROR_PROTOCOL;

    package_count = 0U;
    for (uint32_t offset = sizeof(header) - 1U; offset < length;) {
        uint32_t end = offset;
        while (end < length && catalog_buffer[end] != '\n') end++;
        if (end == offset || package_count >= PACKAGE_MANAGER_MAX_PACKAGES
            || parse_catalog_row(&catalog_buffer[offset], end - offset,
                &package_entries[package_count]) != 0) return PACKAGE_MANAGER_ERROR_PROTOCOL;
        for (uint32_t previous = 0U; previous < package_count; previous++) {
            if (key_matches(package_entries[previous].id, package_entries[previous].version,
                    package_entries[package_count].id, package_entries[package_count].version))
                return PACKAGE_MANAGER_ERROR_PROTOCOL;
        }
        package_count++;
        offset = end < length ? end + 1U : length;
    }
    if (parse_local_records() != 0) return PACKAGE_MANAGER_ERROR_STORAGE;
    annotate_entries();
    return PACKAGE_MANAGER_OK;
}

uint32_t package_manager_count(void) { return package_count; }

int package_manager_get_entry(uint32_t index, package_manager_entry_t* out_entry) {
    if (out_entry == 0 || index >= package_count) return -1;
    *out_entry = package_entries[index];
    return 0;
}

int package_manager_get_details(uint32_t index, char* description, uint32_t capacity) {
    PackageManifest manifest = {0};
    if (index >= package_count || description == 0 || capacity == 0U) return PACKAGE_MANAGER_ERROR_NOT_FOUND;
    int result = fetch_manifest(&package_entries[index], &manifest);
    if (result == PACKAGE_MANAGER_OK) text_copy(description, capacity, manifest.description);
    return result;
}

static int digest_matches(const uint8_t digest[32], const char* expected) {
    static const char digits[] = "0123456789abcdef";
    for (uint32_t i = 0U; i < 32U; i++) {
        if (expected[i * 2U] != digits[digest[i] >> 4]
            || expected[i * 2U + 1U] != digits[digest[i] & 0x0FU]) return 0;
    }
    return 1;
}

static uint32_t read_u32le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint16_t read_u16le(const uint8_t* bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int app_image_valid(const uint8_t* bytes, uint32_t length) {
    if (length < PRISM_APP_HEADER_SIZE || read_u32le(bytes) != PRISM_APP_MAGIC
        || read_u16le(bytes + 4U) != PRISM_APP_FORMAT_VERSION
        || (read_u16le(bytes + 6U) & PRISM_APP_FLAG_DRIVER) != 0U) return 0;
    uint32_t image_size = read_u32le(bytes + 12U);
    uint32_t entry_offset = read_u32le(bytes + 8U);
    return image_size == length - PRISM_APP_HEADER_SIZE && image_size != 0U
        && entry_offset < image_size;
}

static void append_text(char* output, uint32_t capacity, uint32_t* length, const char* text) {
    while (*text != '\0' && *length + 1U < capacity) output[(*length)++] = *text++;
    output[*length] = '\0';
}

static void append_uint(char* output, uint32_t capacity, uint32_t* length, uint32_t value) {
    char reversed[10];
    uint32_t count = 0U;
    do { reversed[count++] = (char)('0' + value % 10U); value /= 10U; } while (value != 0U);
    while (count != 0U && *length + 1U < capacity) output[(*length)++] = reversed[--count];
    output[*length] = '\0';
}

typedef struct { char name[13]; int found; } NameSearch;

static int find_name(const vfs_entry_t* entry, void* context) {
    NameSearch* search = (NameSearch*)context;
    if (text_equal(entry->name, search->name)) { search->found = 1; return 1; }
    return 0;
}

static int allocate_install_path(char path[PACKAGE_MANAGER_PATH_CAPACITY]) {
    NameSearch search;
    for (uint32_t slot = 1U; slot < 10000000U; slot++) {
        search.found = 0;
        search.name[0] = 'P';
        uint32_t value = slot;
        for (int digit = 7; digit >= 1; digit--) {
            search.name[digit] = (char)('0' + value % 10U);
            value /= 10U;
        }
        search.name[8] = '.'; search.name[9] = 'A'; search.name[10] = 'P';
        search.name[11] = 'P'; search.name[12] = '\0';
        if (vfs_list(PACKAGES_DIR, find_name, &search) != 0) return -1;
        if (!search.found) {
            uint32_t length = 0U;
            append_text(path, PACKAGE_MANAGER_PATH_CAPACITY, &length, "/PKGS/");
            append_text(path, PACKAGE_MANAGER_PATH_CAPACITY, &length, search.name);
            return 0;
        }
    }
    return -1;
}

int package_manager_install(uint32_t index) {
    DEBUG_LOG("pkg: installer started");
    if (index >= package_count) {
        DEBUG_LOG("pkg: installer rejected invalid selection");
        return PACKAGE_MANAGER_ERROR_NOT_FOUND;
    }
    if (package_entries[index].installed) {
        DEBUG_LOG("pkg: installer rejected already-installed package");
        return PACKAGE_MANAGER_ERROR_ALREADY_INSTALLED;
    }
    if (package_entries[index].size > PACKAGE_CAPACITY) {
        DEBUG_LOG("pkg: installer rejected package above 64 KiB limit");
        return PACKAGE_MANAGER_ERROR_LIMIT;
    }
    PackageManifest manifest = {0};
    int result = fetch_manifest(&package_entries[index], &manifest);
    if (result != PACKAGE_MANAGER_OK) {
        DEBUG_LOG("pkg: installer could not validate package manifest");
        return result;
    }
    DEBUG_LOG("pkg: manifest validated; starting chunk download");

    sha256_context_t hash;
    sha256_init(&hash);
    uint32_t offset = 0U;
    while (offset < manifest.size) {
        uint32_t requested = manifest.size - offset;
        if (requested > manifest.chunk_size) requested = manifest.chunk_size;
        char path[192];
        uint32_t path_length = 0U;
        append_text(path, sizeof(path), &path_length, "/api/v1/packages/");
        append_text(path, sizeof(path), &path_length, package_entries[index].id);
        append_text(path, sizeof(path), &path_length, "/");
        append_text(path, sizeof(path), &path_length, package_entries[index].version);
        append_text(path, sizeof(path), &path_length, "/chunk?offset=");
        append_uint(path, sizeof(path), &path_length, offset);
        append_text(path, sizeof(path), &path_length, "&length=");
        append_uint(path, sizeof(path), &path_length, requested);
        uint32_t received = 0U;
        result = http_fetch(path, chunk_buffer, CHUNK_CAPACITY, &received, "application/octet-stream");
        if (result != PACKAGE_MANAGER_OK || received != requested) {
            DEBUG_LOG("pkg: chunk request failed or returned an unexpected length");
            return PACKAGE_MANAGER_ERROR_NETWORK;
        }
        for (uint32_t i = 0U; i < received; i++) package_buffer[offset + i] = chunk_buffer[i];
        sha256_update(&hash, chunk_buffer, received);
        offset += received;
    }
    uint8_t digest[32];
    sha256_final(&hash, digest);
    if (!digest_matches(digest, manifest.sha256)) {
        DEBUG_LOG("pkg: downloaded SHA-256 does not match manifest");
        return PACKAGE_MANAGER_ERROR_HASH;
    }
    DEBUG_LOG("pkg: downloaded SHA-256 verified");
    if (!app_image_valid(package_buffer, manifest.size)) {
        DEBUG_LOG("pkg: downloaded artifact is not a supported PrismOS app");
        return PACKAGE_MANAGER_ERROR_APP_FORMAT;
    }
    DEBUG_LOG("pkg: PrismOS app header validated");
    if (parse_local_records() != 0 || installed_count >= PACKAGE_MANAGER_MAX_PACKAGES)
        return PACKAGE_MANAGER_ERROR_STORAGE;

    char path[PACKAGE_MANAGER_PATH_CAPACITY];
    if (allocate_install_path(path) != 0) return PACKAGE_MANAGER_ERROR_STORAGE;
    if (vfs_write_file(path, (const char*)package_buffer, manifest.size, 0) != 0) {
        DEBUG_LOG("pkg: could not write installed app to FAT32");
        return PACKAGE_MANAGER_ERROR_STORAGE;
    }
    InstalledRecord* record = &installed_records[installed_count++];
    text_copy(record->id, sizeof(record->id), package_entries[index].id);
    text_copy(record->version, sizeof(record->version), package_entries[index].version);
    text_copy(record->path, sizeof(record->path), path);
    if (save_installed_records() != 0) {
        installed_count--;
        (void)vfs_rm(path);
        DEBUG_LOG("pkg: registry update failed; removed incomplete installation");
        return PACKAGE_MANAGER_ERROR_STORAGE;
    }
    DEBUG_LOG("pkg: installation completed successfully");
    annotate_entries();
    return PACKAGE_MANAGER_OK;
}

int package_manager_remove(uint32_t index) {
    if (index >= package_count) return PACKAGE_MANAGER_ERROR_NOT_FOUND;
    if (parse_local_records() != 0) return PACKAGE_MANAGER_ERROR_STORAGE;
    for (uint32_t i = 0U; i < installed_count; i++) {
        if (key_matches(package_entries[index].id, package_entries[index].version,
                installed_records[i].id, installed_records[i].version)) {
            InstalledRecord removed = installed_records[i];
            for (uint32_t j = i + 1U; j < installed_count; j++) installed_records[j - 1U] = installed_records[j];
            installed_count--;
            if (save_installed_records() != 0) {
                for (uint32_t j = installed_count; j > i; j--) installed_records[j] = installed_records[j - 1U];
                installed_records[i] = removed;
                installed_count++;
                return PACKAGE_MANAGER_ERROR_STORAGE;
            }
            if (vfs_rm(removed.path) != 0) {
                for (uint32_t j = installed_count; j > i; j--) installed_records[j] = installed_records[j - 1U];
                installed_records[i] = removed;
                installed_count++;
                (void)save_installed_records();
                return PACKAGE_MANAGER_ERROR_STORAGE;
            }
            annotate_entries();
            return PACKAGE_MANAGER_OK;
        }
    }
    return PACKAGE_MANAGER_ERROR_NOT_FOUND;
}

int package_manager_toggle_favorite(uint32_t index) {
    if (index >= package_count || parse_local_records() != 0) return PACKAGE_MANAGER_ERROR_STORAGE;
    for (uint32_t i = 0U; i < favorite_count; i++) {
        if (key_matches(package_entries[index].id, package_entries[index].version,
                favorite_records[i].id, favorite_records[i].version)) {
            FavoriteRecord removed = favorite_records[i];
            for (uint32_t j = i + 1U; j < favorite_count; j++) favorite_records[j - 1U] = favorite_records[j];
            favorite_count--;
            if (save_favorite_records() != 0) {
                for (uint32_t j = favorite_count; j > i; j--) favorite_records[j] = favorite_records[j - 1U];
                favorite_records[i] = removed;
                favorite_count++;
                return PACKAGE_MANAGER_ERROR_STORAGE;
            }
            annotate_entries();
            return PACKAGE_MANAGER_OK;
        }
    }
    if (favorite_count >= PACKAGE_MANAGER_MAX_PACKAGES) return PACKAGE_MANAGER_ERROR_LIMIT;
    text_copy(favorite_records[favorite_count].id, sizeof(favorite_records[favorite_count].id), package_entries[index].id);
    text_copy(favorite_records[favorite_count].version, sizeof(favorite_records[favorite_count].version), package_entries[index].version);
    favorite_count++;
    if (save_favorite_records() != 0) { favorite_count--; return PACKAGE_MANAGER_ERROR_STORAGE; }
    annotate_entries();
    return PACKAGE_MANAGER_OK;
}

const char* package_manager_error_text(int error) {
    switch (error) {
        case PACKAGE_MANAGER_OK: return "Completed successfully.";
        case PACKAGE_MANAGER_ERROR_NETWORK: return "Cannot reach 10.0.2.2:8080. Start backendapi with dotnet run --urls http://0.0.0.0:8080 and verify DHCP/network status.";
        case PACKAGE_MANAGER_ERROR_PROTOCOL: return "Repository response is invalid or incompatible.";
        case PACKAGE_MANAGER_ERROR_STORAGE: return "Filesystem operation failed or package registry is damaged.";
        case PACKAGE_MANAGER_ERROR_LIMIT: return "Package exceeds the 64 KiB install limit or local capacity.";
        case PACKAGE_MANAGER_ERROR_HASH: return "Package SHA-256 did not match the repository manifest; install was refused.";
        case PACKAGE_MANAGER_ERROR_APP_FORMAT: return "Artifact is not a supported PrismOS application package.";
        case PACKAGE_MANAGER_ERROR_NOT_FOUND: return "Select a package first.";
        case PACKAGE_MANAGER_ERROR_ALREADY_INSTALLED: return "This package version is already installed.";
        case PACKAGE_MANAGER_ERROR_DNS: return "DNS failed. Check DHCP/DNS with net status; QEMU repository uses 10.0.2.2.";
        case PACKAGE_MANAGER_ERROR_CONNECT: return "TCP connect failed: start backendapi on the host with dotnet run --urls http://0.0.0.0:8080, then boot with make run-net.";
        case PACKAGE_MANAGER_ERROR_TIMEOUT: return "Repository request timed out. Check host firewall, service, link, and DHCP with net status.";
        default: return "Package operation failed.";
    }
}

int package_manager_list(void) {
    int result = package_manager_refresh();
    if (result != PACKAGE_MANAGER_OK) {
        console_writeln(package_manager_error_text(result));
        return result;
    }
    console_writeln("ID  VERSION  SIZE  STATE  SHA-256");
    if (package_count == 0U) console_writeln("No packages are available.");
    for (uint32_t i = 0U; i < package_count; i++) {
        console_write(package_entries[i].id);
        console_write("  ");
        console_write(package_entries[i].version);
        console_write("  ");
        console_write_uint(package_entries[i].size);
        console_write("  ");
        console_write(package_entries[i].installed ? "INSTALLED" : "available");
        if (package_entries[i].favorite) console_write(" *");
        console_write("  ");
        console_writeln(package_entries[i].sha256);
    }
    return PACKAGE_MANAGER_OK;
}