/*
 * Copyright (C) 2025 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Sierra release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "common.h"

#define SYSMEM_MAP_OP 1
#define SYSMEM_INC_OP 2
#define SYSMEM_SWP_OP 3
#define SYSMEM_IO_READ 4
#define SYSMEM_IO_WRITE 5

extern struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid);
int liballoc(struct pcb_t *, addr_t, uint32_t);
int libfree(struct pcb_t *, uint32_t);
int libread(struct pcb_t*, uint32_t, addr_t, uint32_t*);
int libwrite(struct pcb_t*, BYTE, uint32_t, addr_t);
int libkmem_malloc(struct pcb_t*, uint32_t, uint32_t);
int libkmem_cache_alloc(struct pcb_t *, uint32_t, uint32_t);
int libkmem_cache_pool_create(struct pcb_t*, uint32_t, uint32_t, uint32_t);
int libkmem_copy_from_user(struct pcb_t*, uint32_t, uint32_t, uint32_t, uint32_t);
int libkmem_copy_to_user(struct pcb_t*, uint32_t, uint32_t, uint32_t, uint32_t);

/* Release all per-process resources (frames, page tables, mm, code segment,
 * and the PCB itself). Safe to call when the process has finished. */
void free_pcb(struct pcb_t *proc);
int  free_pcb_memph(struct pcb_t *caller);
