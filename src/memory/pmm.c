#include "pmm.h"

#include <stdint.h>

#include "debug/log.h"

#define PAGE_SIZE 4096U
#define MAX_PHYS_ADDR 0xFFFFFFFFU
#define MAX_PAGES ((MAX_PHYS_ADDR / PAGE_SIZE) + 1U)
#define BITMAP_BYTES (MAX_PAGES / 8U)

#define MULTIBOOT_TAG_TYPE_END         0U
#define MULTIBOOT_TAG_TYPE_MMAP        6U
#define MULTIBOOT_MEMORY_AVAILABLE     1U

#define LOW_MEMORY_RESERVE_END 0x00100000U
#define PHYS_ADDR_LIMIT_PLUS_ONE ((uint64_t)MAX_PHYS_ADDR + 1ULL)

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) multiboot_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} __attribute__((packed)) multiboot_tag_mmap_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} __attribute__((packed)) multiboot_mmap_entry_t;

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

static uint8_t pmm_bitmap[BITMAP_BYTES];
static pmm_stats_t pmm_stats;
static uint32_t pmm_search_hint = 1U;
static int pmm_initialized = 0;

static const multiboot_tag_t *multiboot_next_tag(const multiboot_tag_t *tag)
{
    uintptr_t next = (uintptr_t)tag + ((tag->size + 7U) & ~7U);
    return (const multiboot_tag_t *)next;
}

static uint32_t align_down(uint32_t value, uint32_t align)
{
    return value & ~(align - 1U);
}

static uint32_t align_up(uint32_t value, uint32_t align)
{
    return (value + (align - 1U)) & ~(align - 1U);
}

static void bitmap_set(uint32_t page)
{
    pmm_bitmap[page >> 3] |= (uint8_t)(1U << (page & 7U));
}

static void bitmap_clear(uint32_t page)
{
    pmm_bitmap[page >> 3] &= (uint8_t)~(1U << (page & 7U));
}

static int bitmap_test(uint32_t page)
{
    return (pmm_bitmap[page >> 3] & (uint8_t)(1U << (page & 7U))) != 0;
}

static void reserve_range(uint32_t start_addr, uint32_t end_addr)
{
    /* Reservations are tracked in whole pages: round outwards so partially
     * covered pages are never handed out later. */
    uint32_t start = align_down(start_addr, PAGE_SIZE);
    uint32_t end = align_up(end_addr, PAGE_SIZE);

    if (end <= start) {
        return;
    }

    uint32_t start_page = start / PAGE_SIZE;
    uint32_t end_page = end / PAGE_SIZE;

    if (end_page > MAX_PAGES) {
        end_page = MAX_PAGES;
    }

    for (uint32_t page = start_page; page < end_page; ++page) {
        if (!bitmap_test(page)) {
            bitmap_set(page);
            if (pmm_stats.free_pages > 0U) {
                pmm_stats.free_pages--;
            }
            pmm_stats.used_pages++;
        }
    }
}

static void free_usable_range(uint64_t start_addr, uint64_t end_addr)
{
    if (end_addr <= start_addr) {
        return;
    }

    if (start_addr > (uint64_t)MAX_PHYS_ADDR) {
        return;
    }

    if (end_addr > PHYS_ADDR_LIMIT_PLUS_ONE) {
        end_addr = PHYS_ADDR_LIMIT_PLUS_ONE;
    }

    uint32_t start = align_up((uint32_t)start_addr, PAGE_SIZE);
    uint32_t end = align_down((uint32_t)end_addr, PAGE_SIZE);

    if (end <= start) {
        return;
    }

    uint32_t start_page = start / PAGE_SIZE;
    uint32_t end_page = end / PAGE_SIZE;

    for (uint32_t page = start_page; page < end_page; ++page) {
        if (page == 0U) {
            continue;
        }

        if (bitmap_test(page)) {
            bitmap_clear(page);
            pmm_stats.free_pages++;
            if (pmm_stats.used_pages > 0U) {
                pmm_stats.used_pages--;
            }
        }
    }
}

void pmm_init(const void *multiboot_info, const FramebufferInfo *framebuffer)
{
    const uint8_t *boot_info = (const uint8_t *)multiboot_info;
    const multiboot_tag_t *mmap_tag = 0;
    uint64_t highest_phys = 0;

    /* Start with every page reserved, then explicitly free only Multiboot
     * usable regions. This fail-safe order avoids accidental early reuse of
     * firmware, bootloader, or kernel memory. */
    for (uint32_t i = 0; i < BITMAP_BYTES; ++i) {
        pmm_bitmap[i] = 0xFFU;
    }

    pmm_stats.total_memory_bytes = 0U;
    pmm_stats.usable_memory_bytes = 0U;
    pmm_stats.reserved_memory_bytes = 0U;
    pmm_stats.total_pages = MAX_PAGES;
    pmm_stats.free_pages = 0U;
    pmm_stats.used_pages = MAX_PAGES;

    if (boot_info == 0) {
        ERROR_LOG("pmm_init: multiboot info pointer is null");
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }

    const uint32_t total_size = *(const uint32_t *)boot_info;
    const multiboot_tag_t *tag = (const multiboot_tag_t *)(boot_info + 8U);
    const multiboot_tag_t *end = (const multiboot_tag_t *)(boot_info + total_size);

    while (tag < end && tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            mmap_tag = tag;
            break;
        }
        tag = multiboot_next_tag(tag);
    }

    if (mmap_tag == 0) {
        ERROR_LOG("pmm_init: required multiboot memory map tag missing");
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }

    const multiboot_tag_mmap_t *mmap = (const multiboot_tag_mmap_t *)mmap_tag;
    const uint8_t *entry_ptr = (const uint8_t *)mmap + sizeof(multiboot_tag_mmap_t);
    const uint8_t *mmap_end = (const uint8_t *)mmap + mmap->size;

    while (entry_ptr + mmap->entry_size <= mmap_end) {
        const multiboot_mmap_entry_t *entry = (const multiboot_mmap_entry_t *)entry_ptr;
        uint64_t region_end = entry->addr + entry->len;

        if (region_end > highest_phys) {
            highest_phys = region_end;
        }

        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
            free_usable_range(entry->addr, region_end);
        }

        entry_ptr += mmap->entry_size;
    }

    /* Keep the first MiB reserved for BIOS data, legacy structures and
     * boot-stage compatibility. */
    reserve_range(0U, LOW_MEMORY_RESERVE_END);

    const uint32_t boot_info_start = (uint32_t)(uintptr_t)boot_info;
    const uint32_t boot_info_end = boot_info_start + total_size;
    reserve_range(boot_info_start, boot_info_end);

    const uint32_t kernel_start = (uint32_t)(uintptr_t)&__kernel_start;
    const uint32_t kernel_end = (uint32_t)(uintptr_t)&__kernel_end;
    reserve_range(kernel_start, kernel_end);

    if (framebuffer != 0) {
        uint64_t fb_start64 = framebuffer->address;
        uint64_t fb_size64 = (uint64_t)framebuffer->pitch * (uint64_t)framebuffer->height;
        uint64_t fb_end64 = fb_start64 + fb_size64;

        if (fb_start64 <= (uint64_t)MAX_PHYS_ADDR) {
            if (fb_end64 > ((uint64_t)MAX_PHYS_ADDR + 1ULL)) {
                fb_end64 = (uint64_t)MAX_PHYS_ADDR + 1ULL;
            }
            reserve_range((uint32_t)fb_start64, (uint32_t)fb_end64);
        }
    }

    /* Page frame 0 is never allocatable. */
    bitmap_set(0U);

    if (highest_phys > MAX_PHYS_ADDR) {
        highest_phys = MAX_PHYS_ADDR;
    }

    pmm_stats.total_memory_bytes = (uint32_t)highest_phys;
    pmm_stats.total_pages = pmm_stats.total_memory_bytes / PAGE_SIZE;
    pmm_stats.used_pages = pmm_stats.total_pages - pmm_stats.free_pages;
    pmm_stats.usable_memory_bytes = pmm_stats.free_pages * PAGE_SIZE;
    pmm_stats.reserved_memory_bytes = (pmm_stats.total_pages - pmm_stats.free_pages) * PAGE_SIZE;

    pmm_search_hint = 1U;
    pmm_initialized = 1;

    DEBUG_LOG("pmm_init: initialized");
}

static int pages_are_free(uint32_t start_page, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (bitmap_test(start_page + i)) {
            return 0;
        }
    }
    return 1;
}

static void mark_pages_used(uint32_t start_page, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        bitmap_set(start_page + i);
    }

    if (pmm_stats.free_pages >= count) {
        pmm_stats.free_pages -= count;
    } else {
        pmm_stats.free_pages = 0U;
    }
    pmm_stats.used_pages += count;
}

void *pmm_alloc_pages_below(size_t count, uint64_t max_address_exclusive)
{
    if (!pmm_initialized || count == 0U) {
        return 0;
    }

    uint32_t page_count = (uint32_t)count;
    if (page_count > pmm_stats.free_pages) {
        return 0;
    }

    uint32_t max_page;
    uint32_t start = pmm_search_hint;

    if (max_address_exclusive > PHYS_ADDR_LIMIT_PLUS_ONE) {
        max_address_exclusive = PHYS_ADDR_LIMIT_PLUS_ONE;
    }
    if (max_address_exclusive > ((uint64_t)pmm_stats.total_pages * PAGE_SIZE)) {
        max_address_exclusive = (uint64_t)pmm_stats.total_pages * PAGE_SIZE;
    }
    if (max_address_exclusive < PAGE_SIZE) {
        return 0;
    }
    max_page = (uint32_t)(max_address_exclusive / PAGE_SIZE) - 1U;

    if (start < 1U) {
        start = 1U;
    }
    if (start > max_page) {
        start = 1U;
    }

    for (uint32_t pass = 0; pass < 2U; ++pass) {
        uint32_t begin = (pass == 0U) ? start : 1U;
        uint32_t end = (pass == 0U) ? max_page : start - 1U;

        if (begin > end || page_count > (end - begin + 1U)) {
            continue;
        }

        uint32_t last_start = end - page_count + 1U;
        for (uint32_t page = begin; page <= last_start; ++page) {
            if (!pages_are_free(page, page_count)) {
                continue;
            }

            mark_pages_used(page, page_count);
            pmm_search_hint = page + page_count;
            return (void *)(uintptr_t)(page * PAGE_SIZE);
        }
    }

    return 0;
}

void *pmm_alloc_pages(size_t count)
{
    return pmm_alloc_pages_below(count, PHYS_ADDR_LIMIT_PLUS_ONE);
}

void *pmm_alloc_page(void)
{
    return pmm_alloc_pages(1U);
}

void pmm_free_page(void *addr)
{
    pmm_free_pages(addr, 1U);
}

void pmm_free_pages(void *addr, size_t count)
{
    if (!pmm_initialized || addr == 0 || count == 0U || count > 0xFFFFFFFFU) {
        return;
    }

    uint32_t physical = (uint32_t)(uintptr_t)addr;
    if ((physical & (PAGE_SIZE - 1U)) != 0U) {
        return;
    }

    uint32_t start_page = physical / PAGE_SIZE;
    uint32_t page_count = (uint32_t)count;
    if (start_page == 0U || start_page >= pmm_stats.total_pages
        || page_count > pmm_stats.total_pages - start_page) {
        return;
    }

    for (uint32_t offset = 0U; offset < page_count; ++offset) {
        uint32_t page = start_page + offset;
        if (bitmap_test(page)) {
            bitmap_clear(page);
            pmm_stats.free_pages++;
            if (pmm_stats.used_pages > 0U) {
                pmm_stats.used_pages--;
            }

            if (page < pmm_search_hint) {
                pmm_search_hint = page;
            }
        }
    }
}

uint32_t pmm_total_memory_bytes(void)
{
    return pmm_stats.total_memory_bytes;
}

const pmm_stats_t *pmm_get_stats(void)
{
    return &pmm_stats;
}
