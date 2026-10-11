#include "paging.h"

#include <stdint.h>

#include "debug/log.h"
#include "display/console.h"
#include "interrupts/irq.h"
#include "platform/io.h"

#define PAGE_SIZE 4096U
#define PAGE_ENTRIES 1024U
#define BOOTSTRAP_PT_COUNT 128U
#define IDENTITY_BOOT_END 0x04000000U

#define PAGE_DIR_INDEX(x) (((x) >> 22) & 0x3FFU)
#define PAGE_TABLE_INDEX(x) (((x) >> 12) & 0x3FFU)
#define PAGE_ALIGN_DOWN(x) ((x) & 0xFFFFF000U)
#define USER_PAGE_DIRECTORY_INDEX PAGE_DIR_INDEX(PAGING_USER_BASE)

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

static uint32_t page_directory[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint32_t user_page_directories[PAGING_USER_SPACE_MAX][PAGE_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));
static uint32_t user_page_tables[PAGING_USER_SPACE_MAX][PAGE_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));
static uint8_t user_page_space_used[PAGING_USER_SPACE_MAX];
static uint32_t kernel_page_directory_physical;
static int active_user_page_space = -1;
/* Early boot uses a static page-table pool to avoid allocator recursion while
 * PMM and paging are brought up together. */
static uint32_t page_table_pool[BOOTSTRAP_PT_COUNT][PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint32_t page_table_pool_used = 0U;

static int paging_enabled = 0;
static int paging_fault_handler_registered = 0;

static void memory_zero(void *ptr, uint32_t size)
{
    uint8_t *out = (uint8_t *)ptr;
    for (uint32_t i = 0; i < size; ++i) {
        out[i] = 0;
    }
}

static void page_fault_handler(registers_t *regs)
{
    /* Keep this simple for now; richer diagnostics and recovery can be added
     * once fault handling policies are introduced. */
    (void)regs;
    (void)read_cr2();
    ERROR_LOG("page_fault_handler: page fault detected");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static uint32_t *ensure_page_table(uint32_t virt)
{
    uint32_t dir_index = PAGE_DIR_INDEX(virt);

    if ((page_directory[dir_index] & PAGING_FLAG_PRESENT) == 0U) {
        if (page_table_pool_used >= BOOTSTRAP_PT_COUNT) {
            ERROR_LOG("paging: out of memory allocating page table");
            return 0;
        }

        uint32_t *table_virt = page_table_pool[page_table_pool_used++];
        uint32_t table_phys = (uint32_t)(uintptr_t)table_virt;
        memory_zero(table_virt, PAGE_SIZE);

        page_directory[dir_index] = table_phys | PAGING_FLAG_PRESENT | PAGING_FLAG_READ_WRITE;
    }

    return (uint32_t *)(uintptr_t)(page_directory[dir_index] & 0xFFFFF000U);
}

void paging_map(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t *table = ensure_page_table(virt);
    if (table == 0) {
        return;
    }

    uint32_t table_index = PAGE_TABLE_INDEX(virt);
    table[table_index] = PAGE_ALIGN_DOWN(phys) | (flags & 0xFFFU) | PAGING_FLAG_PRESENT;

    if (paging_enabled) {
        invlpg((void *)(uintptr_t)PAGE_ALIGN_DOWN(virt));
    }
}

void paging_unmap(uint32_t virt)
{
    uint32_t dir_index = PAGE_DIR_INDEX(virt);
    if ((page_directory[dir_index] & PAGING_FLAG_PRESENT) == 0U) {
        return;
    }

    uint32_t *table = (uint32_t *)(uintptr_t)(page_directory[dir_index] & 0xFFFFF000U);
    table[PAGE_TABLE_INDEX(virt)] = 0U;

    if (paging_enabled) {
        invlpg((void *)(uintptr_t)PAGE_ALIGN_DOWN(virt));
    }
}

uint32_t paging_get_physical(uint32_t virt)
{
    uint32_t dir_index = PAGE_DIR_INDEX(virt);
    if ((page_directory[dir_index] & PAGING_FLAG_PRESENT) == 0U) {
        return 0U;
    }

    uint32_t *table = (uint32_t *)(uintptr_t)(page_directory[dir_index] & 0xFFFFF000U);
    uint32_t entry = table[PAGE_TABLE_INDEX(virt)];

    if ((entry & PAGING_FLAG_PRESENT) == 0U) {
        return 0U;
    }

    return (entry & 0xFFFFF000U) | (virt & 0xFFFU);
}

static int paging_user_space_valid(const paging_user_space_t* space)
{
    return space != 0
        && space->slot < PAGING_USER_SPACE_MAX
        && user_page_space_used[space->slot] != 0U;
}

int paging_user_space_is_active(const paging_user_space_t* space)
{
    return paging_user_space_valid(space)
        && active_user_page_space == (int)space->slot;
}

static int paging_user_space_copy_valid(const paging_user_space_t* space,
    uint32_t address, uint32_t length, int write_to_user)
{
    uint64_t end = (uint64_t)address + length;

    if (!paging_user_space_is_active(space)
        || address < PAGING_USER_BASE
        || end > (uint64_t)PAGING_USER_BASE + PAGING_USER_SIZE) {
        return 0;
    }

    while (address < end) {
        uint32_t page_index = PAGE_TABLE_INDEX(address);
        uint32_t entry = user_page_tables[space->slot][page_index];
        uint32_t page_end = (address & 0xFFFFF000U) + PAGE_SIZE;

        if ((entry & (PAGING_FLAG_PRESENT | PAGING_FLAG_USER))
                != (PAGING_FLAG_PRESENT | PAGING_FLAG_USER)
                || (write_to_user != 0 && (entry & PAGING_FLAG_READ_WRITE) == 0U)) {
            return 0;
        }

        address = page_end < end ? page_end : (uint32_t)end;
    }

    return 1;
}

int paging_user_space_create(paging_user_space_t* out_space)
{
    if (out_space == 0 || !paging_enabled) {
        return -1;
    }

    for (uint8_t slot = 0U; slot < PAGING_USER_SPACE_MAX; ++slot) {
        if (user_page_space_used[slot] != 0U) {
            continue;
        }

        for (uint32_t index = 0U; index < PAGE_ENTRIES; ++index) {
            user_page_directories[slot][index] = page_directory[index];
            user_page_tables[slot][index] = 0U;
        }

        user_page_directories[slot][USER_PAGE_DIRECTORY_INDEX] = 0U;
        user_page_space_used[slot] = 1U;
        out_space->slot = slot;
        return 0;
    }

    return -1;
}

int paging_user_space_map(const paging_user_space_t* space,
    uint32_t virtual_address, uint32_t physical_address, uint32_t flags)
{
    uint32_t page_index;
    uint32_t directory_index;
    uint32_t* directory;
    uint32_t* table;

    if (!paging_user_space_valid(space)
        || (virtual_address & (PAGE_SIZE - 1U)) != 0U
        || (physical_address & (PAGE_SIZE - 1U)) != 0U
        || virtual_address < PAGING_USER_BASE
        || virtual_address >= PAGING_USER_BASE + PAGING_USER_SIZE
        || (flags & ~PAGING_FLAG_READ_WRITE) != 0U) {
        return -1;
    }

    directory = user_page_directories[space->slot];
    directory_index = PAGE_DIR_INDEX(virtual_address);
    if (directory_index != USER_PAGE_DIRECTORY_INDEX) {
        return -1;
    }

    directory[directory_index] = (uint32_t)(uintptr_t)
        user_page_tables[space->slot]
        | PAGING_FLAG_PRESENT | PAGING_FLAG_READ_WRITE | PAGING_FLAG_USER;
    table = user_page_tables[space->slot];
    page_index = PAGE_TABLE_INDEX(virtual_address);
    if ((table[page_index] & PAGING_FLAG_PRESENT) != 0U) {
        return -1;
    }

    table[page_index] = physical_address | flags
        | PAGING_FLAG_PRESENT | PAGING_FLAG_USER;
    if (active_user_page_space == (int)space->slot && paging_enabled) {
        invlpg((void*)(uintptr_t)virtual_address);
    }
    return 0;
}

int paging_user_space_copy_from(const paging_user_space_t* space,
    void* destination, uint32_t user_source, uint32_t length)
{
    volatile const uint8_t* source;
    uint8_t* output = (uint8_t*)destination;

    if ((destination == 0 && length != 0U)
        || !paging_user_space_copy_valid(space, user_source, length, 0)) {
        return -1;
    }

    source = (volatile const uint8_t*)(uintptr_t)user_source;
    for (uint32_t index = 0U; index < length; ++index) {
        output[index] = source[index];
    }
    return 0;
}

int paging_user_space_copy_to(const paging_user_space_t* space,
    uint32_t user_destination, const void* source, uint32_t length)
{
    volatile uint8_t* destination;
    const uint8_t* input = (const uint8_t*)source;

    if ((source == 0 && length != 0U)
        || !paging_user_space_copy_valid(space, user_destination, length, 1)) {
        return -1;
    }

    destination = (volatile uint8_t*)(uintptr_t)user_destination;
    for (uint32_t index = 0U; index < length; ++index) {
        destination[index] = input[index];
    }
    return 0;
}

void paging_user_space_destroy(const paging_user_space_t* space)
{
    if (!paging_user_space_valid(space)) {
        return;
    }

    if (active_user_page_space == (int)space->slot) {
        paging_kernel_space_activate();
    }

    for (uint32_t index = 0U; index < PAGE_ENTRIES; ++index) {
        user_page_directories[space->slot][index] = 0U;
        user_page_tables[space->slot][index] = 0U;
    }
    user_page_space_used[space->slot] = 0U;
}

int paging_user_space_activate(const paging_user_space_t* space)
{
    if (!paging_user_space_valid(space)) {
        return -1;
    }

    active_user_page_space = (int)space->slot;
    write_cr3((uint32_t)(uintptr_t)user_page_directories[space->slot]);
    return 0;
}

void paging_kernel_space_activate(void)
{
    active_user_page_space = -1;
    write_cr3(kernel_page_directory_physical);
}

static void map_identity_range(uint32_t start, uint32_t end, uint32_t flags)
{
    uint32_t current = PAGE_ALIGN_DOWN(start);
    uint32_t final = (end + PAGE_SIZE - 1U) & 0xFFFFF000U;

    while (current < final) {
        paging_map(current, current, flags);
        current += PAGE_SIZE;
    }
}

void paging_init(const FramebufferInfo *framebuffer)
{
    memory_zero(page_directory, sizeof(page_directory));
    page_table_pool_used = 0U;

    /* Identity-map a conservative early region so existing physical-pointer
     * code continues to run after CR0.PG is enabled. */
    map_identity_range(0x00000000U, IDENTITY_BOOT_END, PAGING_FLAG_READ_WRITE);

    uint32_t kernel_start = (uint32_t)(uintptr_t)&__kernel_start;
    uint32_t kernel_end = (uint32_t)(uintptr_t)&__kernel_end;
    map_identity_range(kernel_start, kernel_end, PAGING_FLAG_READ_WRITE);

    if (framebuffer != 0) {
        uint64_t fb_start64 = framebuffer->address;
        uint64_t fb_size64 = (uint64_t)framebuffer->pitch * (uint64_t)framebuffer->height;
        uint64_t fb_end64 = fb_start64 + fb_size64;

        if (fb_start64 <= 0xFFFFFFFFULL) {
            if (fb_end64 > 0x100000000ULL) {
                fb_end64 = 0x100000000ULL;
            }
            map_identity_range((uint32_t)fb_start64, (uint32_t)fb_end64, PAGING_FLAG_READ_WRITE);
        }
    }

    for (uint32_t page_directory_index = 0U; page_directory_index < 4U;
        ++page_directory_index) {
        ensure_page_table(0xE1000000U + page_directory_index * 0x00400000U);
        ensure_page_table(0xF0000000U + page_directory_index * 0x00400000U);
    }

    write_cr3((uint32_t)(uintptr_t)page_directory);
    kernel_page_directory_physical = (uint32_t)(uintptr_t)page_directory;
    write_cr0(read_cr0() | 0x80000000U);

    paging_enabled = 1;

    if (!paging_fault_handler_registered) {
        exception_register_handler(14U, page_fault_handler);
        paging_fault_handler_registered = 1;
    }

    DEBUG_LOG("paging_init: paging enabled");
}
