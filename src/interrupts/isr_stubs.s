/* isr_stubs.s — ISR and IRQ entry stubs for PrismOS (32-bit AT&T syntax)
 *
 * Each stub pushes a dummy error code (where the CPU does not) and the
 * interrupt/IRQ number onto the stack, then falls into a common handler
 * that saves all registers and calls the C dispatcher.
 *
 * CPU exceptions with a hardware error code pushed by the CPU:
 *   8 (double fault), 10–14, 17 (alignment check).
 * All other exception stubs push a dummy 0 first so the stack layout is
 * always the same.
 *
 * IRQ stubs push a dummy 0 and the vector number (32 + IRQ line).
 */

.section .text

/* =========================================================================
 * Macros
 * ====================================================================== */

/* CPU exception — NO hardware error code */
.macro ISR_NO_ERR num
.global isr\num
isr\num:
    pushl $0          /* dummy error code */
    pushl $\num
    jmp   isr_common_stub
.endm

/* CPU exception — CPU DOES push an error code */
.macro ISR_WITH_ERR num
.global isr\num
isr\num:
    pushl $\num       /* error code already on stack from CPU */
    jmp   isr_common_stub
.endm

/* Hardware IRQ — vector = 32 + irq_line */
.macro IRQ_STUB irq_line
.global irq\irq_line
irq\irq_line:
    pushl $0
    pushl $(32 + \irq_line)
    jmp   irq_common_stub
.endm

/* =========================================================================
 * CPU exception stubs (vectors 0–31)
 * ====================================================================== */
ISR_NO_ERR   0    /* Divide by zero              */
ISR_NO_ERR   1    /* Debug                       */
ISR_NO_ERR   2    /* NMI                         */
ISR_NO_ERR   3    /* Breakpoint                  */
ISR_NO_ERR   4    /* Overflow                    */
ISR_NO_ERR   5    /* Bound range exceeded        */
ISR_NO_ERR   6    /* Invalid opcode              */
ISR_NO_ERR   7    /* Device not available        */
ISR_WITH_ERR 8    /* Double fault                */
ISR_NO_ERR   9    /* Coprocessor segment overrun */
ISR_WITH_ERR 10   /* Invalid TSS                 */
ISR_WITH_ERR 11   /* Segment not present         */
ISR_WITH_ERR 12   /* Stack fault                 */
ISR_WITH_ERR 13   /* General protection fault    */
ISR_WITH_ERR 14   /* Page fault                  */
ISR_NO_ERR   15   /* Reserved                    */
ISR_NO_ERR   16   /* x87 FPU error               */
ISR_WITH_ERR 17   /* Alignment check             */
ISR_NO_ERR   18   /* Machine check               */
ISR_NO_ERR   19   /* SIMD FPU exception          */
ISR_NO_ERR   20   /* Virtualisation              */
ISR_NO_ERR   21
ISR_NO_ERR   22
ISR_NO_ERR   23
ISR_NO_ERR   24
ISR_NO_ERR   25
ISR_NO_ERR   26
ISR_NO_ERR   27
ISR_NO_ERR   28
ISR_NO_ERR   29
ISR_NO_ERR   30
ISR_NO_ERR   31

/* =========================================================================
 * Hardware IRQ stubs (IRQ lines 0–15, vectors 0x20–0x2F)
 * ====================================================================== */
IRQ_STUB  0    /* PIT timer        */
IRQ_STUB  1    /* PS/2 keyboard    */
IRQ_STUB  2    /* Cascade (slave)  */
IRQ_STUB  3    /* COM2             */
IRQ_STUB  4    /* COM1             */
IRQ_STUB  5    /* LPT2 / soundcard */
IRQ_STUB  6    /* Floppy           */
IRQ_STUB  7    /* LPT1 / spurious  */
IRQ_STUB  8    /* RTC              */
IRQ_STUB  9    /* Free / ACPI      */
IRQ_STUB  10   /* Free             */
IRQ_STUB  11   /* Free             */
IRQ_STUB  12   /* PS/2 mouse       */
IRQ_STUB  13   /* FPU              */
IRQ_STUB  14   /* Primary ATA      */
IRQ_STUB  15   /* Secondary ATA    */

/* DPL-3 system call gate. The syscall number is supplied in EAX. */
.global isr128
isr128:
    pushl $0
    pushl $128
    pusha
    movw  %ds, %ax
    pushl %eax

    movw  $0x10, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs

    pushl %esp
    call  syscall_dispatcher
    addl  $4, %esp

    popl  %eax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs
    popa
    addl  $8, %esp
    iret

/* =========================================================================
 * Common ISR stub — saves context, calls isr_handler(registers_t *)
 * ====================================================================== */
isr_common_stub:
    pusha                       /* save edi,esi,ebp,esp,ebx,edx,ecx,eax */
    movw  %ds,   %ax
    pushl %eax                  /* save data segment selector            */

    movw  $0x10, %ax            /* load kernel data segment              */
    movw  %ax,   %ds
    movw  %ax,   %es
    movw  %ax,   %fs
    movw  %ax,   %gs

    pushl %esp                  /* arg: pointer to registers_t           */
    call  isr_handler
    addl  $4,    %esp

    popl  %eax                  /* restore data segment                  */
    movw  %ax,   %ds
    movw  %ax,   %es
    movw  %ax,   %fs
    movw  %ax,   %gs

    popa
    addl  $8,    %esp           /* discard err_code + int_no             */
    iret

/* =========================================================================
 * Common IRQ stub — saves context, calls irq_dispatcher(registers_t *)
 * ====================================================================== */
irq_common_stub:
    pusha
    movw  %ds,   %ax
    pushl %eax

    movw  $0x10, %ax
    movw  %ax,   %ds
    movw  %ax,   %es
    movw  %ax,   %fs
    movw  %ax,   %gs

    pushl %esp
    call  irq_dispatcher
    addl  $4,    %esp

    popl  %eax
    movw  %ax,   %ds
    movw  %ax,   %es
    movw  %ax,   %fs
    movw  %ax,   %gs

    popa
    addl  $8,    %esp
    iret
