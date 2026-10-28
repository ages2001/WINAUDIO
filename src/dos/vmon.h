/* vmon.h - the V86 monitor of the WINAUDIO DOS core (vmon.c, vmonent.asm) and its DPMI host
   (dpmi.c). DOS runs in virtual 8086 mode under the core, so the Sound Blaster ports are
   trapped without EMM386 or JEMM; protected-mode programs get VCPI and DPMI from here. */
#ifndef VMON_H
#define VMON_H

/* the register frame of every entry into the monitor (vmonent.asm builds it on the ring-0
   stack): PUSHAD image, vector, error code, the CPU frame. es..gs: V86 mode, the CPU pushed
   them; protected mode, the selectors of the client (loaded again on the way out). */
struct vmregs {
    u32 edi, esi, ebp, kesp, ebx, edx, ecx, eax;
    u32 vec, err;
    u32 eip, cs, eflags, esp, ss;
    u32 es, ds, fs, gs;
};

#define EFL_CF      0x0001
#define EFL_PF      0x0004
#define EFL_AF      0x0010
#define EFL_ZF      0x0040
#define EFL_SF      0x0080
#define EFL_TF      0x0100
#define EFL_IF      0x0200
#define EFL_DF      0x0400
#define EFL_OF      0x0800
#define EFL_IOPL    0x3000
#define EFL_NT      0x4000
#define EFL_RF      0x10000
#define EFL_VM      0x20000
#define EFL_AC      0x40000
#define EFL_ARITH   (EFL_CF | EFL_PF | EFL_AF | EFL_ZF | EFL_SF | EFL_OF)

/* GDT of the monitor */
#define SEL_CODE32  0x08
#define SEL_DATA32  0x10
#define SEL_CODE16  0x18
#define SEL_DATA16  0x20
#define SEL_TSS     0x28
#define SEL_LDT     0x30
#define SEL_STUB32  0x3B        /* ring 3: the host's protected-mode stubs (HLT traps) */
#define SEL_STUB16  0x43
#define SEL_LPMS32  0x4B        /* ring 3: locked protected-mode stack */
#define SEL_LPMS16  0x53
#define SEL_RMSS    0x5B        /* ring 3: real-mode stack of a callback (DS:ESI) */
#define SEL_FLAT3   0x63        /* ring 3: flat data (not given to clients) */
#define SEL_ESPFIX  0x68        /* ring 0: stack of the IRETD to a 16-bit ring-3 stack */
#define VM_NGDT     14

/* stub page offsets (HLT, except RETF) */
#define STUB_IRET   0x00        /* end of an interrupt handler called by the host */
#define STUB_EXC    0x01        /* end of an exception handler (RETF) */
#define STUB_RAWRM  0x02        /* raw switch protected -> real mode */
#define STUB_RETF   0x03        /* RETF: state save / restore */
#define STUB_CB     0x04        /* end of a real-mode callback (IRET) */
#define STUB_DEF    0x100       /* + vector: default handler of a protected-mode interrupt */

/* hypercalls (ARPL in V86 mode, 63h C0h+n, from WINAUDIO.SYS only) */
#define HC_CORE     0
#define HC_RET      1
#define HC_DPMI     2
#define HC_RAWPM    3
#define HC_TOREAL   4
#define HC_CB       16
#define VM_NCB      32

#define CORE_WIN    0x80000000UL

/* vmonent.asm */
extern u32 vm_esp0;
#pragma aux vm_esp0 "_*";
extern u32 vm_nest;
#pragma aux vm_nest "_*";
extern u8 vm_gdtr[6];
#pragma aux vm_gdtr "_*";
extern u8 vm_idtr[6];
#pragma aux vm_idtr "_*";
extern u32 vm_tssp;
#pragma aux vm_tssp "_*";
extern u32 vm_tssdesc;
#pragma aux vm_tssdesc "_*";
extern u32 vm_ldtsel;
#pragma aux vm_ldtsel "_*";
extern u32 vm_cr4;
#pragma aux vm_cr4 "_*";
extern u32 vm_rmptr[2];
#pragma aux vm_rmptr "_*";
extern u8 vm_stubs[];
#pragma aux vm_stubs "_*";
extern u8 vm_exit[];
#pragma aux vm_exit "_*";
extern u8 vm_espbuf[];
#pragma aux vm_espbuf "_*";
u32 __cdecl vm_enter(struct vmregs *r);         /* run *r until its return trap; *r = the state then */
void __cdecl vm_resume(struct vmregs *f);       /* the innermost vm_enter returns with *f */
void __cdecl vm_jump(struct vmregs *f, u32 depth);  /* drop the calls above depth, go on with *f */
void __cdecl vm_launch(struct vmregs *f);       /* first entry into V86 mode */
int __cdecl vm_halt(void);                      /* STI, HLT: the vector of the interrupt */
void __cdecl vm_toreal(void);                   /* far jump to the 16-bit code in WINAUDIO.SYS */
void __cdecl vm_golin(u32 lin);                 /* near jump to a linear address (VCPI gateway) */
int __cdecl vm_rdmsr(u32 msr, u32 *lo, u32 *hi);  /* -1: the CPU refused */
int __cdecl vm_wrmsr(u32 msr, u32 lo, u32 hi);

u32 rd_cr0(void);
#pragma aux rd_cr0 = "mov eax,cr0" value [eax] modify exact [eax];
void wr_cr0(u32 v);
#pragma aux wr_cr0 = "mov cr0,eax" parm [eax] modify exact [];
u32 rd_cr2(void);
#pragma aux rd_cr2 = "mov eax,cr2" value [eax] modify exact [eax];
u32 rd_cr3(void);
#pragma aux rd_cr3 = "mov eax,cr3" value [eax] modify exact [eax];
void wr_cr3(u32 v);
#pragma aux wr_cr3 = "mov cr3,eax" parm [eax] modify exact [];
u32 rd_cr4(void);
#pragma aux rd_cr4 = "mov eax,cr4" value [eax] modify exact [eax];
void wr_cr4(u32 v);
#pragma aux wr_cr4 = "mov cr4,eax" parm [eax] modify exact [];
void invlpg(u32 lin);
#pragma aux invlpg = "invlpg [eax]" parm [eax] modify exact [];
void cpu_clts(void);
#pragma aux cpu_clts = "clts" modify exact [];
void cpu_invd(void);
#pragma aux cpu_invd = "invd" modify exact [];
u32 cpu_lar(u32 sel);
#pragma aux cpu_lar = "lar eax,eax" "jz ok" "xor eax,eax" "ok:" parm [eax] value [eax] modify exact [eax];

/* vmon.c */
extern struct winvm *vmc;
extern u32 drv_lin;
extern u8 vm_mbase, vm_sbase;       /* interrupt vectors of the two PICs */
u32 lin_rd32(u32 lin);
u16 lin_rd16(u32 lin);
u8 lin_rd8(u32 lin);
void lin_wr32(u32 lin, u32 v);
void lin_wr16(u32 lin, u16 v);
void lin_wr8(u32 lin, u8 v);
int lin_ok(u32 lin, u32 len);           /* reachable by the client (conventional memory, DPMI blocks) */
void v86_push(struct vmregs *f, u16 v);
u16 v86_pop(struct vmregs *f);
void v86_reflect(struct vmregs *f, int vec);    /* real-mode interrupt in V86 mode */
int rm_stack(u32 *ss, u32 *sp);                 /* stack for a real-mode call of the host */
int rm_setup(struct vmregs *t, struct vmregs *f);   /* real-mode context for a call made while f was trapped */
void rmstk_push(u32 ss, u32 sp);                /* V86 code interrupted by protected-mode code */
void rmstk_pop(void);
void rm_call(struct vmregs *t, int how);        /* how: 0 far call, 1 interrupt (CS:IP from t) */
void rm_int(struct vmregs *t, int vec);         /* real-mode interrupt with t's registers */
int io_insn(struct vmregs *f, u32 csbase, int def32);  /* trapped IN / OUT: 1 handled */
void vm_hlt(struct vmregs *f);
void irq_entry(struct vmregs *f, int vec);      /* hardware interrupt, any mode */
int irq_of(int vec);                            /* IRQ of a vector, -1 none */
int irq_vec(int irq);                           /* real-mode vector of an IRQ */
void vm_setredir(int vec, int on);              /* VME: software interrupt goes to the monitor */
void vm_kill(struct vmregs *f, const char *msg);  /* a program did something the monitor cannot do */
void vm_print(struct vmregs *f, const char *s);    /* text through the BIOS */
void __cdecl vm_panic(u32 *p);
u64 mkdesc(u32 base, u32 limit, u32 access, u32 flags);
void pm_call(struct vmregs *t);                 /* run protected-mode code of the client until its return stub */
void pm_return(struct vmregs *f);               /* its return stub was reached */
int rmstk_depth(void);
void rmstk_set(int n);
int dpmi_mapped(u32 lin, u32 len);

/* dpmi.c */
extern struct dpmicl *dcl;                      /* the running client, NULL none */
u32 dpmi_setup(u32 *ldt, u32 stubs, u32 lpms);  /* LDT entries, 0: no DPMI */
int dpmi_hoststack(u32 *ss, u32 *sp);           /* the client's real-mode host stack */
void dpmi_kill(struct vmregs *f);               /* the client ends (message already shown) */
void dpmi_rmirq(struct vmregs *f, int vec);     /* hardware interrupt in protected mode to real mode */
int dpmi_hooked(int vec);                       /* the client has a protected-mode handler */
void dpmi_enter(struct vmregs *f);              /* HC_DPMI */
void dpmi_rawpm(struct vmregs *f);              /* HC_RAWPM */
void dpmi_callback(struct vmregs *f, int i);    /* HC_CB + i */
void pm_trap(struct vmregs *f);                 /* entry from protected mode (ring 3) */
int dpmi_hwint(struct vmregs *f, int vec);      /* hardware interrupt to a protected-mode handler: 1 done */
int dpmi_rmint(struct vmregs *f, int vec);      /* INT 1Ch / 23h / 24h in real mode to protected mode */
void dpmi_rmexit(struct vmregs *f);             /* INT 21h AH=4Ch in real mode */
u32 sel_base(u32 sel, int *big, u32 *limit);    /* -1: not usable */
u32 dpmi_lin_end(void);

#endif
