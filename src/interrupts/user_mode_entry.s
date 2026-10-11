.section .text

.global user_mode_enter_asm
.global user_mode_return_trampoline
.extern user_mode_return_stack

user_mode_enter_asm:
    pushl %ebp
    movl  %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi

    movl  8(%ebp), %eax
    movl  %esp, (%eax)
    movl  12(%ebp), %edx
    movl  16(%ebp), %ecx

    pushl $0x23
    pushl %ecx
    pushfl
    popl  %ecx
    andl  $0xFFFF8FFF, %ecx
    orl   $0x202, %ecx
    pushl %ecx
    pushl $0x1B
    pushl $user_mode_user_trampoline
    iret

.section .user_vm.text,"ax"
.global user_mode_user_trampoline
user_mode_user_trampoline:
    movw $0x23, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    jmp *%edx

.section .text
user_mode_return_trampoline:
    movl  user_mode_return_stack, %esp
    popl  %edi
    popl  %esi
    popl  %ebx
    popl  %ebp
    ret

.section .note.GNU-stack,"",@progbits
