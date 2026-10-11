#ifndef PRISMOS_PAGING_H
#define PRISMOS_PAGING_H

#include <stdint.h>

#include "display/console.h"

#define PAGING_FLAG_PRESENT       0x001U
#define PAGING_FLAG_READ_WRITE    0x002U
#define PAGING_FLAG_USER          0x004U
#define PAGING_FLAG_WRITE_THROUGH 0x008U
#define PAGING_FLAG_CACHE_DISABLE 0x010U
#define PAGING_FLAG_GLOBAL        0x100U

#define PAGING_USER_SPACE_MAX 4U
#define PAGING_USER_BASE 0x40000000U
#define PAGING_USER_SIZE 0x00400000U

typedef struct {
    uint8_t slot;
} paging_user_space_t;

/* Enable early x86 paging. This implementation uses identity mapping in
 * the bootstrap phase. */
void paging_init(const FramebufferInfo *framebuffer);

/* Map/unmap virtual pages to physical pages using 4 KiB granularity.
 * Addresses should be page-aligned by callers for predictable behavior. */
void paging_map(uint32_t virt, uint32_t phys, uint32_t flags);
void paging_unmap(uint32_t virt);

uint32_t paging_get_physical(uint32_t virt);

int paging_user_space_create(paging_user_space_t* out_space);
int paging_user_space_is_active(const paging_user_space_t* space);
int paging_user_space_map(const paging_user_space_t* space,
    uint32_t virtual_address, uint32_t physical_address, uint32_t flags);
int paging_user_space_copy_from(const paging_user_space_t* space,
    void* destination, uint32_t user_source, uint32_t length);
int paging_user_space_copy_to(const paging_user_space_t* space,
    uint32_t user_destination, const void* source, uint32_t length);
void paging_user_space_destroy(const paging_user_space_t* space);
int paging_user_space_activate(const paging_user_space_t* space);
void paging_kernel_space_activate(void);

#endif /* PRISMOS_PAGING_H */
