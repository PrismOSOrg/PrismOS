#ifndef PRISMOS_COMMAND_H
#define PRISMOS_COMMAND_H

#include <stdint.h>

typedef struct {
	const char* name;
	const char* description;
	const char* usage;
	const char* parameters;
	const char* example;
} command_help_entry_t;

void command_execute(const char* line);
void command_print_help(void);
const char* command_get_cwd(void);
uint32_t command_get_help_count(void);
const command_help_entry_t* command_get_help_entry(uint32_t index);

#endif