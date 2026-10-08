#ifndef PRISMOS_IO_H
#define PRISMOS_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint32_t read_cr0(void) {
    uint32_t value;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(value));
    return value;
}

static inline void write_cr0(uint32_t value) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(value) : "memory");
}

static inline uint32_t read_cr2(void) {
    uint32_t value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline uint32_t read_cr3(void) {
    uint32_t value;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void write_cr3(uint32_t value) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline void invlpg(const void *addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline void cpu_relax(void) {
    __asm__ volatile ("pause");
}

static inline void reboot_system(void) {
    __asm__ volatile ("cli");
    outb(0x64, 0xFE);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static inline void shutdown_system(void) {
    __asm__ volatile ("cli");
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

#endif