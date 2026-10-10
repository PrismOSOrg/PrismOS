#ifndef PRISMOS_APPS_PACKAGE_MANAGER_H
#define PRISMOS_APPS_PACKAGE_MANAGER_H

#include <stdint.h>

#define PACKAGE_MANAGER_MAX_PACKAGES 64U
#define PACKAGE_MANAGER_ID_CAPACITY 33U
#define PACKAGE_MANAGER_VERSION_CAPACITY 33U
#define PACKAGE_MANAGER_HASH_CAPACITY 65U
#define PACKAGE_MANAGER_PATH_CAPACITY 24U

typedef struct {
	char id[PACKAGE_MANAGER_ID_CAPACITY];
	char version[PACKAGE_MANAGER_VERSION_CAPACITY];
	char sha256[PACKAGE_MANAGER_HASH_CAPACITY];
	char installed_path[PACKAGE_MANAGER_PATH_CAPACITY];
	uint32_t size;
	uint8_t installed;
	uint8_t favorite;
} package_manager_entry_t;

enum {
	PACKAGE_MANAGER_OK = 0,
	PACKAGE_MANAGER_ERROR_NETWORK = -1,
	PACKAGE_MANAGER_ERROR_PROTOCOL = -2,
	PACKAGE_MANAGER_ERROR_STORAGE = -3,
	PACKAGE_MANAGER_ERROR_LIMIT = -4,
	PACKAGE_MANAGER_ERROR_HASH = -5,
	PACKAGE_MANAGER_ERROR_APP_FORMAT = -6,
	PACKAGE_MANAGER_ERROR_NOT_FOUND = -7,
	PACKAGE_MANAGER_ERROR_ALREADY_INSTALLED = -8,
	PACKAGE_MANAGER_ERROR_DNS = -9,
	PACKAGE_MANAGER_ERROR_CONNECT = -10,
	PACKAGE_MANAGER_ERROR_TIMEOUT = -11
};

int package_manager_refresh(void);
uint32_t package_manager_count(void);
int package_manager_get_entry(uint32_t index, package_manager_entry_t* out_entry);
int package_manager_get_details(uint32_t index, char* description, uint32_t capacity);
int package_manager_install(uint32_t index);
int package_manager_remove(uint32_t index);
int package_manager_toggle_favorite(uint32_t index);
const char* package_manager_error_text(int error);
int package_manager_list(void);
void package_manager_run(void);

#endif