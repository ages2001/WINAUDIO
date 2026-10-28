; vmonent.asm - low-level part of the V86 monitor of the WINAUDIO DOS core (vmon.c, dpmi.c)
;
; Every interrupt and exception comes through one of 256 stubs into vm_entry, which builds
; a struct vmregs on the ring-0 stack and calls vm_trap. The frame is told apart by its size
; below the ring-0 stack top (TSS.ESP0): 36 bytes V86 mode, 20 bytes ring 3, plus 4 with an
; error code. 16 bytes are kept free above ESP0, where a ring-3 frame keeps the client's
; segment registers, so one frame can be turned from one mode into the other.
;
; vm_enter / vm_resume run code in V86 mode or ring 3 on behalf of C code that waits for it
; (a real-mode call of the host, an interrupt handler of a DPMI client): the C context stays
; on the ring-0 stack and ESP0 is moved below it for the time of the call.

        bits 32

segment _TEXT public class=CODE use32 align=16

SEL_CODE32      equ 08h
SEL_DATA32      equ 10h
SEL_CODE16      equ 18h
SEL_TSS         equ 28h
EFL_VM          equ 20000h
NESTMAX         equ 40
SEL_ESPFIX      equ 68h
ESPOFF          equ 20h

extern _vm_trap
extern _vm_panic

global _vm_esp0, _vm_nest, _vm_gdtr, _vm_idtr, _vm_tssp, _vm_tssdesc, _vm_ldtsel, _vm_cr4
global _vm_rmptr, _vm_stubs, _vm_exit, _vm_inhlt, _vm_hltvec
global _vm_enter, _vm_resume, _vm_jump, _vm_launch, _vm_halt, _vm_toreal, _vm_golin
global _vm_rdmsr, _vm_wrmsr, _vm_espbuf

                align 4
_vm_esp0        dd 0            ; TSS.ESP0 (16 bytes below the free top)
_vm_tssp        dd 0            ; the TSS
_vm_tssdesc     dd 0            ; its descriptor in the GDT (busy bit)
_vm_ldtsel      dd 0
_vm_cr4         dd 0            ; CR4 of the monitor, 0: the CPU has none
_vm_nest        dd 0            ; calls waiting in vm_enter
_vm_hltvec      dd 0
_vm_guard       dd 0            ; ring-0 fault: continue here (RDMSR / WRMSR)
_vm_rmptr       dd 0            ; 16:32 pointer to the 16-bit code of WINAUDIO.SYS
                dw SEL_CODE16
_vm_inhlt       db 0
                align 4
_vm_gdtr        dw 0
                dd 0
                align 4
_vm_idtr        dw 0
                dd 0
                align 4
nsp             times NESTMAX dd 0      ; ESP of each waiting vm_enter
nreg            times NESTMAX dd 0      ; its struct vmregs
nesp0           times NESTMAX dd 0      ; ESP0 before it
jbuf            times 19 dd 0
espsel          dw SEL_ESPFIX
                align 16
_vm_espbuf      times 8 dd 0            ; IRETD frame to a 16-bit ring-3 stack

; ---------------------------------------------------------------------------
; 256 entry stubs, 10 bytes each
                align 16
_vm_stubs:
%assign i 0
%rep 256
        push strict dword i
        jmp strict near vm_entry
%assign i i+1
%endrep

vm_entry:
        push eax
        cmp byte [ss:_vm_inhlt], 0
        jne .halted
        mov eax, [ss:_vm_esp0]
        sub eax, esp
        cmp eax, 8 + 36
        je .noerr
        cmp eax, 8 + 20
        je .noerr
        cmp eax, 8 + 40
        je .err
        cmp eax, 8 + 24
        je .err
        pop eax
        jmp r0fault
        ; an interrupt while the monitor waits in vm_halt (ring 0, no error code)
.halted:
        mov eax, [esp+4]
        mov [ss:_vm_hltvec], eax
        pop eax
        add esp, 4
        and dword [esp+8], ~200h        ; back with interrupts disabled
        iretd
.noerr: sub esp, 4                      ; room for an error code of 0
        mov eax, [esp+4]
        mov [esp], eax
        mov eax, [esp+8]
        mov [esp+4], eax
        mov dword [esp+8], 0
        jmp .frame
.err:   or dword [esp+4], 100h          ; vector bit 8: the CPU pushed an error code
.frame: pop eax
        pushad
        test dword [esp+48], EFL_VM
        jnz .seg
        xor eax, eax                    ; ring 3: the client's selectors into the frame
        mov ax, es
        mov [esp+60], eax
        mov ax, ds
        mov [esp+64], eax
        mov ax, fs
        mov [esp+68], eax
        mov ax, gs
        mov [esp+72], eax
.seg:   mov ax, SEL_DATA32
        mov ds, ax
        mov es, ax
        xor eax, eax
        mov fs, ax
        mov gs, ax
        cld
        push esp
        call _vm_trap
        add esp, 4
_vm_exit:
        test dword [esp+48], EFL_VM
        jnz .out
        mov eax, [esp+60]
        call selok
        mov es, ax
        mov eax, [esp+68]
        call selok
        mov fs, ax
        mov eax, [esp+72]
        call selok
        mov gs, ax
        mov eax, [esp+64]
        call selok
        mov ds, ax
        mov eax, [esp+56]               ; a 16-bit stack segment: the CPU leaves the high word
        lar eax, ax                     ; of ESP from ring 0, so IRETD goes from a stack whose
        jnz .out                        ; ESP is small (espfix)
        test eax, 400000h
        jnz .out
        mov eax, [esp+40]
        mov [ss:_vm_espbuf], eax
        mov eax, [esp+44]
        mov [ss:_vm_espbuf+4], eax
        mov eax, [esp+48]
        mov [ss:_vm_espbuf+8], eax
        mov eax, [esp+52]
        and eax, 0FFFFh
        mov [ss:_vm_espbuf+12], eax
        mov eax, [esp+56]
        mov [ss:_vm_espbuf+16], eax
        popad
        mov ss, [cs:espsel]
        mov esp, ESPOFF
        iretd
.out:   popad
        add esp, 8
        iretd

; AX = selector: AX, or 0 if it cannot be loaded into a data segment register for ring 3
selok:  test eax, 0FFFCh
        jz .null
        mov edx, eax
        lar edx, dx
        jnz .null
        test edx, 8000h                 ; present
        jz .null
        test edx, 1000h                 ; code or data
        jz .null
        test edx, 800h
        jz .dpl
        test edx, 200h                  ; code: readable
        jz .null
.dpl:   shr edx, 13
        and edx, 3
        cmp edx, 3                      ; DPL 3 only
        jne .null
        ret
.null:  xor eax, eax
        ret

; a fault inside the monitor: [esp] = vector, then the CPU frame
r0fault:
        cmp dword [esp], 2              ; NMI
        je .nmi
        cmp dword [ss:_vm_guard], 0
        jne .guard
        push eax
        mov ax, SEL_DATA32
        mov ds, ax
        mov es, ax
        pop eax
        push esp
        call _vm_panic
.h:     cli
        hlt
        jmp .h
.nmi:   add esp, 4
        iretd
.guard: mov eax, [esp]                  ; vector 13 has an error code, 6 has none
        cmp eax, 13
        jne .g1
        add esp, 4
.g1:    add esp, 4
        mov eax, [ss:_vm_guard]
        mov [esp], eax                  ; EIP
        mov dword [ss:_vm_guard], 0
        iretd

; ---------------------------------------------------------------------------
; vm_setesp0: EAX = new ESP0
setesp0:
        mov [_vm_esp0], eax
        mov edx, [_vm_tssp]
        mov [edx+4], eax
        ret

; frame position for ESP0 = EAX and the frame at ESI: EDI
framepos:
        mov edi, eax
        sub edi, 76
        test dword [esi+48], EFL_VM
        jnz .v
        add edi, 16
.v:     ret

; u32 vm_enter(struct vmregs *r)
_vm_enter:
        push ebp
        push ebx
        push esi
        push edi
        mov esi, [esp+20]
        mov edx, [_vm_nest]
        cmp edx, NESTMAX
        jae .full
        mov [nsp+edx*4], esp
        mov [nreg+edx*4], esi
        mov eax, [_vm_esp0]
        mov [nesp0+edx*4], eax
        inc edx
        mov [_vm_nest], edx
        lea eax, [esp-32]
        and eax, ~15
        call setesp0
        call framepos
        mov esp, edi
        mov ecx, 19
        rep movsd
        jmp _vm_exit
.full:  pop edi
        pop esi
        pop ebx
        pop ebp
        mov eax, -1
        ret

; void vm_resume(struct vmregs *f)
_vm_resume:
        mov esi, [esp+4]
        mov edx, [_vm_nest]
        test edx, edx
        jz .none
        dec edx
        mov [_vm_nest], edx
        mov edi, [nreg+edx*4]
        mov ecx, 19
        rep movsd
        mov eax, [nesp0+edx*4]
        mov ebx, [nsp+edx*4]
        call setesp0
        mov esp, ebx
        pop edi
        pop esi
        pop ebx
        pop ebp
        xor eax, eax
        ret
.none:  ret

; void vm_jump(struct vmregs *f, u32 depth)
_vm_jump:
        mov esi, [esp+4]
        mov edx, [esp+8]
        mov edi, jbuf
        mov ecx, 19
        rep movsd
        cmp edx, [_vm_nest]
        jae .same
        mov [_vm_nest], edx
        mov eax, [nesp0+edx*4]
        call setesp0
.same:  mov eax, [_vm_esp0]
        mov esi, jbuf
        call framepos
        mov esp, edi
        mov ecx, 19
        rep movsd
        jmp _vm_exit

; void vm_launch(struct vmregs *f): the monitor's tables, then V86 mode
_vm_launch:
        mov esi, [esp+4]
        push dword 2                    ; no NT (IRET would switch tasks), IF = 0
        popfd
        lgdt [_vm_gdtr]
        lidt [_vm_idtr]
        jmp SEL_CODE32:.cs
.cs:    mov ax, SEL_DATA32
        mov ds, ax
        mov es, ax
        mov ss, ax
        xor eax, eax
        mov fs, ax
        mov gs, ax
        mov edx, [_vm_tssdesc]
        and byte [edx+5], 0FDh
        mov ax, SEL_TSS
        ltr ax
        mov eax, [_vm_ldtsel]
        lldt ax
        mov eax, [_vm_cr4]
        test eax, eax
        jz .nocr4
        mov cr4, eax
.nocr4: mov dword [_vm_nest], 0
        mov edi, jbuf
        mov ecx, 19
        rep movsd
        mov eax, [_vm_esp0]
        mov esi, jbuf
        call framepos
        mov esp, edi
        mov ecx, 19
        rep movsd
        jmp _vm_exit

; int vm_halt(void)
_vm_halt:
        mov dword [_vm_hltvec], -1
        mov byte [_vm_inhlt], 1
        sti
        hlt
        cli
        mov byte [_vm_inhlt], 0
        mov eax, [_vm_hltvec]
        ret

; void vm_toreal(void): to the 16-bit code of WINAUDIO.SYS, which leaves protected mode
_vm_toreal:
        cli
        jmp far [_vm_rmptr]

; void vm_golin(u32 lin)
_vm_golin:
        mov eax, [esp+4]
        jmp eax

; int vm_rdmsr(u32 msr, u32 *lo, u32 *hi)
_vm_rdmsr:
        push ebx
        mov ecx, [esp+8]
        mov dword [_vm_guard], .bad
        rdmsr
        mov dword [_vm_guard], 0
        mov ebx, [esp+12]
        mov [ebx], eax
        mov ebx, [esp+16]
        mov [ebx], edx
        xor eax, eax
        pop ebx
        ret
.bad:   mov eax, -1
        pop ebx
        ret

; int vm_wrmsr(u32 msr, u32 lo, u32 hi)
_vm_wrmsr:
        mov ecx, [esp+4]
        mov eax, [esp+8]
        mov edx, [esp+12]
        mov dword [_vm_guard], .bad
        wrmsr
        mov dword [_vm_guard], 0
        xor eax, eax
        ret
.bad:   mov eax, -1
        ret
