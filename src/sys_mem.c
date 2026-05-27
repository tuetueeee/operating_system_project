/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "syscall.h"
#include "os-mm.h"
#include "libmem.h"
#include "queue.h"
#include "sched.h"
#include <stdlib.h>
#include <stdio.h>

#ifdef MM64
#include "mm64.h"
#else
#include "mm.h"
#endif

int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs)
{
    int memop = regs->a1;
    BYTE value;

    /*
     * @bksysnet: dual-space design — syscalls run in kernel space and the
     * user side may only hand over a PID. Resolve PID -> PCB via the
     * scheduler's locked helper so concurrent add_proc/remove_proc on
     * other CPUs can't shift the running_list underneath us.
     */
    struct pcb_t *caller = sched_find_proc_by_pid(krnl, pid);
    if (caller == NULL)
    {
        printf("Loi: Khong tim thay tien trinh voi PID %d trong running_list\n", pid);
        return -1;
    }

    /* Syscall operations routing */
    switch (memop)
    {
    case SYSMEM_MAP_OP:
        return vmap_pgd_memset(caller, regs->a2, regs->a3);
    case SYSMEM_INC_OP:
        return inc_vma_limit(caller, regs->a2, regs->a3);
    case SYSMEM_SWP_OP:
        return __mm_swap_page(caller, regs->a2, regs->a3);
    case SYSMEM_IO_READ:
        if (MEMPHY_read(caller->krnl->mram, regs->a2, &value) != 0)
            return -1;
        regs->a3 = value;
        return 0;
    case SYSMEM_IO_WRITE:
        return MEMPHY_write(caller->krnl->mram, regs->a2, regs->a3);
    default:
        printf("Loi: Ma memop khong hop le: %d\n", memop);
        return -1;
    }
}