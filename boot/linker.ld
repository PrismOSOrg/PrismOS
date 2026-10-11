ENTRY(start)

SECTIONS
{
    __kernel_start = .;

    .multiboot : {
        KEEP(*(.multiboot))
        KEEP(*(.multiboot2_header))
    }

    . = 1M;

    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }

    .user_vm 0x40000000 : AT(ALIGN(LOADADDR(.bss) + SIZEOF(.bss), 0x1000))
    {
        __user_vm_start = .;
        *(.user_vm.text*)
        *(.user_vm.rodata*)
        *(.user_vm.data*)
        . = ALIGN(0x1000);
        __user_vm_end = .;
    }

    __user_vm_load_start = LOADADDR(.user_vm);
    __kernel_end = LOADADDR(.user_vm) + SIZEOF(.user_vm);
}
