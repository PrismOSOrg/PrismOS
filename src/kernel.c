#include <stdint.h>

#include "debug/log.h"
#include "display/console.h"
#include "display/psf_font.h"
#include "shell/shell.h"
#include "interrupts/interrupts.h"
#include "input/keyboard.h"
#include "comport/comport.h"
#include "memory/pmm.h"
#include "memory/paging.h"
#include "filesystem/blockdev.h"
#include "filesystem/partition_manager.h"
#include "filesystem/vfs.h"
#include "apps/app_manager.h"

#include <stddef.h>
#include "display/serial.h"

extern unsigned int g_multiboot_magic;
extern unsigned int g_multiboot_info;
extern const uint8_t _binary_assets_fonts_cp850_8x16_psf_start[];
extern const uint8_t _binary_assets_fonts_cp850_8x16_psf_end[];

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289U

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    uint8_t red_position;
    uint8_t red_mask_size;
    uint8_t green_position;
    uint8_t green_mask_size;
    uint8_t blue_position;
    uint8_t blue_mask_size;
} __attribute__((packed));

static void kernel_halt(void) {
    while (1) {
    }
}

static const struct multiboot_tag* multiboot2_next_tag(const struct multiboot_tag* tag) {
    uintptr_t next = (uintptr_t)tag + ((tag->size + 7U) & ~7U);
    return (const struct multiboot_tag*)next;
}

void main(void) {
    // First log point confirms kernel reached C entry.
    DEBUG_LOG("kernel entry");
    unsigned int magic = g_multiboot_magic;
    const uint8_t* boot_info = (const uint8_t*)(uintptr_t)g_multiboot_info;
    const uint8_t* font_start = _binary_assets_fonts_cp850_8x16_psf_start;
    const uint8_t* font_end = _binary_assets_fonts_cp850_8x16_psf_end;
    const uint32_t font_size = (uint32_t)(font_end - font_start);
    FramebufferInfo framebuffer = {0};
    int framebuffer_found = 0;

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        ERROR_LOG("invalid multiboot magic");
        kernel_halt();
    }

    if (boot_info != 0) {
        const uint32_t total_size = *(const uint32_t*)boot_info;
        const struct multiboot_tag* tag = (const struct multiboot_tag*)(boot_info + 8U);
        const struct multiboot_tag* end = (const struct multiboot_tag*)(boot_info + total_size);

        while (tag < end && tag->type != 0U) {
            if (tag->type == 8U) {
                const struct multiboot_tag_framebuffer* framebuffer_tag = (const struct multiboot_tag_framebuffer*)tag;

                /* Accept framebuffer provided by bootloader regardless of type.
                 * For RGB (type 1) we also capture color mask positions.
                 */
                framebuffer.address = framebuffer_tag->address;
                framebuffer.width = framebuffer_tag->width;
                framebuffer.height = framebuffer_tag->height;
                framebuffer.pitch = framebuffer_tag->pitch;
                framebuffer.bytes_per_pixel = (framebuffer_tag->bpp > 0) ? (uint8_t)((framebuffer_tag->bpp + 7U) / 8U) : 1U;

                if (framebuffer_tag->framebuffer_type == 1U) {
                    framebuffer.red_position = framebuffer_tag->red_position;
                    framebuffer.red_mask_size = framebuffer_tag->red_mask_size;
                    framebuffer.green_position = framebuffer_tag->green_position;
                    framebuffer.green_mask_size = framebuffer_tag->green_mask_size;
                    framebuffer.blue_position = framebuffer_tag->blue_position;
                    framebuffer.blue_mask_size = framebuffer_tag->blue_mask_size;
                } else {
                    /* For non-RGB framebuffers, choose a reasonable default mapping. */
                    framebuffer.red_position = 16;
                    framebuffer.red_mask_size = 8;
                    framebuffer.green_position = 8;
                    framebuffer.green_mask_size = 8;
                    framebuffer.blue_position = 0;
                    framebuffer.blue_mask_size = 8;
                }

                framebuffer_found = 1;
            }

            tag = multiboot2_next_tag(tag);
        }
    }

    if (!framebuffer_found) {
        ERROR_LOG("no framebuffer tag found");
        /* Initialize serial early so we have output on UEFI/modern machines */
        serial_init();
        serial_write("No framebuffer found. Using serial fallback for diagnostics.\n");

        if (boot_info == 0) {
            serial_write("multiboot info pointer is NULL\n");
            kernel_halt();
        }

        const uint32_t total_size = *(const uint32_t*)boot_info;
        serial_write("multiboot total_size: ");
        serial_write_hex(total_size);
        serial_write("\n");

        const struct multiboot_tag* tag = (const struct multiboot_tag*)(boot_info + 8U);
        const struct multiboot_tag* end = (const struct multiboot_tag*)(boot_info + total_size);

        while (tag < end && tag->type != 0U) {
            serial_write("tag type: ");
            serial_write_hex(tag->type);
            serial_write("\n");
            tag = multiboot2_next_tag(tag);
        }

        kernel_halt();
    }

    if (!psf_font_init(font_start, font_size)) {
        WARNINIG_LOG("PSF font load failed, using fallback");
        serial_init();
        serial_write("PSF load failed, using built-in fallback font\n");
    }

    // Console init is a key transition from boot parsing to interactive output.
    DEBUG_LOG("initializing console");
    console_init(&framebuffer);

    pmm_init(boot_info, &framebuffer);
    paging_init(&framebuffer);

    // Shell startup marks readiness for user commands.
    DEBUG_LOG("starting shell loop");
    interrupts_init();
    keyboard_init();
    comport_irq_init();

    if (blockdev_init_disk() != 0) {
        ERROR_LOG("storage init failed");
        console_writeln("Storage initialization failed");
    } else if (partition_manager_init() != 0) {
        ERROR_LOG("partition scan failed");
        console_writeln("No supported FAT32 partition found");
    } else if (vfs_init() != 0) {
        ERROR_LOG("filesystem init failed");
        console_writeln("Filesystem initialization failed");
    } else {
        DEBUG_LOG("filesystem mounted");
        if (app_manager_init() != 0) {
            ERROR_LOG("app manager init failed");
            console_writeln("Application manager initialization failed");
        }
    }

    shell_run();
}

void __cxa_pure_virtual() { }

void _exit(void) { kernel_halt(); }