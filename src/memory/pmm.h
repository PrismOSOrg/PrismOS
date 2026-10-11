#ifndef PRISMOS_PMM_H
#define PRISMOS_PMM_H

#include <stddef.h>
#include <stdint.h>

#include "display/console.h"

typedef struct {
    uint32_t total_memory_bytes;
    uint32_t usable_memory_bytes;
    uint32_t reserved_memory_bytes;
    uint32_t total_pages;
    uint32_t free_pages;
    uint32_t used_pages;
} pmm_stats_t;

/* Initialize the physical memory manager from Multiboot2 memory-map data.
 * This must run before paging_init(). */
void pmm_init(const void *multiboot_info, const FramebufferInfo *framebuffer);

/* Allocate / free physical page frames. Returned values are physical
 * addresses represented as pointers for convenience in this pre-VM stage. */
void *pmm_alloc_page(void);
void *pmm_alloc_pages(size_t count);
void *pmm_alloc_pages_below(size_t count, uint64_t max_address_exclusive);
void pmm_free_page(void *addr);
void pmm_free_pages(void *addr, size_t count);

uint32_t pmm_total_memory_bytes(void);
const pmm_stats_t *pmm_get_stats(void);

#endif /* PRISMOS_PMM_H */
