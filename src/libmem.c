/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

// #ifdef MM_PAGING
/*
 * System Library
 * Memory Module Library libmem.c
 */

#include "string.h"
#include "mm.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;

/* Defined in the kernel-memory subsystem further below. */
int kmem_addr_is_kernel(addr_t addr);

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  struct vm_rg_struct *rg_node = mm->mmap->vm_freerg_list;

  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  if (rg_node != NULL)
    rg_elmt->rg_next = rg_node;

  /* Enlist the new region */
  mm->mmap->vm_freerg_list = rg_elmt;

  return 0;
}

/*get_symrg_byid - get mem region by region ID
 *@mm: memory region
 *@rgid: region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return NULL;

  return &mm->symrgtbl[rgid];
}

/*__alloc - allocate a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *@alloc_addr: address of allocated memory region
 *
 */
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (cur_vma == NULL)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;

    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  /* No free region big enough: extend the VMA via SYSMEM_INC_OP. The
   * kernel handler aligns the request to the page size and maps the
   * newly-reserved range to physical frames.
   */
  addr_t old_sbrk = cur_vma->sbrk;

  struct sc_regs regs;
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
  regs.a3 = PAGING_PAGE_ALIGNSZ(size);
  if (_syscall(caller->krnl, caller->pid, 17, &regs) != 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  caller->mm->symrgtbl[rgid].rg_start = old_sbrk;
  caller->mm->symrgtbl[rgid].rg_end = old_sbrk + size;

  *alloc_addr = old_sbrk;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*__free - remove a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
  pthread_mutex_lock(&mmvm_lock);

  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* TODO: Manage the collect freed region to freerg_list */
  struct vm_rg_struct *rgnode = get_symrg_byid(caller->mm, rgid);

  if (rgnode->rg_start == 0 && rgnode->rg_end == 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* Kernel-space regions (kmalloc, kmem_cache_alloc) live in a separate
   * pool with a bump allocator; they must not be enlisted back into the
   * per-VMA free list, which would corrupt user-space bookkeeping. Drop
   * the symbol-table entry and let the kernel pool keep the slot. */
  if (kmem_addr_is_kernel(rgnode->rg_start))
  {
    rgnode->rg_start = rgnode->rg_end = 0;
    rgnode->rg_next = NULL;
    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  freerg_node->rg_start = rgnode->rg_start;
  freerg_node->rg_end = rgnode->rg_end;
  freerg_node->rg_next = NULL;

  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->rg_next = NULL;

  /*enlist the obsoleted memory region */
  enlist_vm_freerg_list(caller->mm, freerg_node);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  addr_t addr;
  int val = __alloc(proc, 0, reg_index, size, &addr);
  if (val == -1)
    return -1;
#ifdef IODUMP
  printf("PID=%u alloc region=%u size=%lu byte at virt addr=%lu\n",
         proc->pid, reg_index, (unsigned long)size, (unsigned long)addr);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif
  MEMPHY_dump(proc->krnl->mram);
#endif

  return val;
}

/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  int val = __free(proc, 0, reg_index);
  if (val == -1)
    return -1;
#ifdef IODUMP
  printf("PID=%u free region=%u\n", proc->pid, reg_index);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif
#endif
  return 0;
}

/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */
int pg_getpage(struct mm_struct *mm, int pgn, int *fpn, struct pcb_t *caller)
{
  uint32_t pte = pte_get_entry(caller, pgn);

  if (PAGING_PAGE_PRESENT(pte))
  {
    *fpn = PAGING_FPN(pte);
    return 0;
  }

  /* Page is not in RAM. Two possibilities:
   *  - the page has been swapped out (PAGING_PTE_SWAPPED_MASK bit set),
   *    in which case we swap it back in;
   *  - the PTE is zero / not swapped, meaning the page was never mapped.
   *    Refuse the access rather than fabricating a frame from swap slot 0.
   */
  if (!(pte & PAGING_PTE_SWAPPED_MASK))
    return -1;

  addr_t vicpgn = 0;
  addr_t swpfpn = 0;

  /* Pick a victim page to evict from RAM. */
  if (find_victim_page(caller->mm, &vicpgn) == -1)
    return -1;

  /* Reserve a free swap slot to hold the evicted RAM frame. If the swap
   * device is full we have to put the victim back into the FIFO list so
   * it can be picked again later, otherwise it would silently disappear
   * from page-replacement tracking.
   */
  if (MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) == -1)
  {
    enlist_pgn_node(&caller->mm->fifo_pgn, vicpgn);
    return -1;
  }

  uint32_t vicpte = pte_get_entry(caller, vicpgn);
  addr_t vicfpn = PAGING_FPN(vicpte);

  int tgt_swptyp = caller->krnl->active_mswp_id;
  addr_t tgt_swpoff = PAGING_SWP(pte);

  /* Swap victim frame from RAM to SWAP via SYSMEM_SWP_OP. */
  struct sc_regs regs;
  regs.a1 = SYSMEM_SWP_OP;
  regs.a2 = vicfpn;
  regs.a3 = swpfpn;
  if (_syscall(caller->krnl, caller->pid, 17, &regs) != 0)
  {
    /* Roll back: release the swap slot we reserved and restore the
     * victim's FIFO bookkeeping. The victim PTE is untouched so its
     * data is still valid in RAM.
     */
    MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
    enlist_pgn_node(&caller->mm->fifo_pgn, vicpgn);
    return -1;
  }

  /* Swap target frame from SWAP back to the now-free RAM frame. */
  __swap_cp_page(caller->krnl->active_mswp, tgt_swpoff,
                 caller->krnl->mram, vicfpn);

  /* Release the swap slot that the target page used to live in. */
  MEMPHY_put_freefp(caller->krnl->active_mswp, tgt_swpoff);

  /* Update PTEs: victim is now in swap, target page is now in vicfpn. */
  pte_set_swap(caller, vicpgn, tgt_swptyp, swpfpn);
  pte_set_fpn(caller, pgn, vicfpn);

  /* Track newly-online page for future replacement decisions. */
  enlist_pgn_node(&caller->mm->fifo_pgn, pgn);

  *fpn = (int)vicfpn;
  return 0;
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_getval(struct mm_struct *mm, addr_t addr, BYTE *data, struct pcb_t *caller)
{
  addr_t pgn = addr / PAGING_PAGESZ;
  addr_t off = addr % PAGING_PAGESZ;
  int fpn;
  struct sc_regs regs;

  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  /* Compute physical address: frame number * page size + offset */
  addr_t phyaddr = ((addr_t)fpn) * PAGING_PAGESZ + off;

  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = phyaddr;
  regs.a3 = 0;
  if (_syscall(caller->krnl, caller->pid, 17, &regs) < 0)
    return -1;

  *data = (BYTE)regs.a3;
  return 0;
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, addr_t addr, BYTE value, struct pcb_t *caller)
{
  addr_t pgn = addr / PAGING_PAGESZ;
  addr_t off = addr % PAGING_PAGESZ;
  int fpn;
  struct sc_regs regs;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  addr_t phyaddr = ((addr_t)fpn) * PAGING_PAGESZ + off;

  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = phyaddr;
  regs.a3 = value;
  if (_syscall(caller->krnl, caller->pid, 17, &regs) < 0)
    return -1;

  return 0;
}

/* Resolve a (region, offset) pair into a user virtual address while
 * holding mmvm_lock. On success returns 0 with mmvm_lock still held —
 * the caller must release it after the page-level op. On any validation
 * failure returns -1 with the lock released.
 */
static int __resolve_rg_addr(struct pcb_t *caller, int rgid, addr_t offset,
                             addr_t *out)
{
  pthread_mutex_lock(&mmvm_lock);

  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  if (currg == NULL ||
      currg->rg_start == currg->rg_end ||
      offset >= currg->rg_end - currg->rg_start)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  *out = currg->rg_start + offset;
  return 0;
}

/*__read - read value in region memory
 *@caller: caller
 *@vmaid: ID vm area (unused — kept for API compatibility)
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to access in memory region
 *@data: out parameter for the byte read
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  (void)vmaid;
  if (caller == NULL || caller->mm == NULL || data == NULL)
    return -1;

  addr_t addr;
  if (__resolve_rg_addr(caller, rgid, offset, &addr) != 0)
    return -1;

  int ret = pg_getval(caller->mm, addr, data, caller);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc,
    uint32_t source,
    addr_t offset,
    uint32_t *destination)
{
  BYTE data;
  int val = __read(proc, 0, source, offset, &data);

  if (val == 0)
    *destination = (uint32_t)data;
#ifdef IODUMP
  printf("PID=%u read region=%u offset=%lu value=%u\n",
         proc->pid, source, (unsigned long)offset, (unsigned)data);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif
#endif

  return val;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area (unused — kept for API compatibility)
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to access in memory region
 *@value: byte value to write
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  (void)vmaid;
  if (caller == NULL || caller->mm == NULL)
    return -1;

  addr_t addr;
  if (__resolve_rg_addr(caller, rgid, offset, &addr) != 0)
    return -1;

  int ret = pg_setval(caller->mm, addr, value, caller);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,
    BYTE data,
    uint32_t destination,
    addr_t offset)
{
  int val = __write(proc, 0, destination, offset, data);
  if (val == -1)
    return -1;
#ifdef IODUMP
  printf("PID=%u write region=%u offset=%lu value=%u\n",
         proc->pid, destination, (unsigned long)offset, (unsigned)data);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif
#endif

  return val;
}


/* ===========================================================================
 * Kernel-memory subsystem (simulated)
 *
 * The simulator backs kernel-space with a fixed-size byte array.
 *   - libkmem_malloc and libkmem_cache_pool_create carve sub-regions from
 *     this pool with a bump allocator. Reclaim is not implemented — that is
 *     adequate for the short-lived simulation workloads.
 *   - Kernel virtual addresses are encoded as KMEM_VBASE + offset so they
 *     stand apart from user-space addresses (which start at 0). libfree
 *     uses this range check to skip enlisting kernel regions back into the
 *     per-VMA free list.
 *   - kmem_cache_alloc bumps a per-pool slot pointer; each slot occupies
 *     `align` bytes (the slab object size).
 * =========================================================================*/

#define KMEM_POOL_SIZE       (1U << 16)              /* 64KB kernel storage */
#define KMEM_VBASE           ((addr_t)1ULL << 56)    /* canonical kernel base */
#define KMEM_MAX_POOLS       32

struct kmem_cache_pool_state {
  int    used;
  int    size;
  int    align;
  addr_t storage;
  int    next_slot;
};

static BYTE                          kmem_storage[KMEM_POOL_SIZE];
static int                           kmem_brk = 0;
static struct kmem_cache_pool_state  kmem_pools[KMEM_MAX_POOLS];
static pthread_mutex_t               kmem_lock = PTHREAD_MUTEX_INITIALIZER;

int kmem_addr_is_kernel(addr_t addr)
{
  return addr >= KMEM_VBASE && addr < KMEM_VBASE + KMEM_POOL_SIZE;
}

static int __kmem_offset(addr_t addr)
{
  return (int)(addr - KMEM_VBASE);
}

/* Bump-allocate a contiguous kernel chunk. Caller must hold kmem_lock.
 * Returns kernel virtual address on success, 0 on exhaustion. */
static addr_t __kmem_carve(int size)
{
  if (size <= 0 || kmem_brk + size > (int)KMEM_POOL_SIZE)
    return 0;
  addr_t out = KMEM_VBASE + (addr_t)kmem_brk;
  kmem_brk += size;
  return out;
}

/*libkmem_malloc - allocate a physically contiguous kernel region
 *@caller: caller
 *@size: region size
 *@reg_index: symbol-table index that records the allocation
 */
int libkmem_malloc(struct pcb_t *caller, uint32_t size, uint32_t reg_index)
{
  if (caller == NULL || caller->mm == NULL || reg_index >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  pthread_mutex_lock(&kmem_lock);
  addr_t addr = __kmem_carve((int)size);
  pthread_mutex_unlock(&kmem_lock);

  if (addr == 0)
    return -1;

  caller->mm->symrgtbl[reg_index].rg_start = addr;
  caller->mm->symrgtbl[reg_index].rg_end   = addr + size;
  caller->mm->symrgtbl[reg_index].rg_next  = NULL;

#ifdef IODUMP
  printf("PID=%u kmalloc region=%u size=%u byte at kernel addr=0x%lx\n",
         caller->pid, reg_index, (unsigned)size, (unsigned long)addr);
#endif
  return 0;
}

/*libkmem_cache_pool_create - reserve a slab of kernel memory for fixed-size objects
 *@caller: caller
 *@size: total slab size
 *@align: per-slot size (object size class)
 *@cache_pool_id: pool ID
 */
int libkmem_cache_pool_create(struct pcb_t *caller, uint32_t size,
                              uint32_t align, uint32_t cache_pool_id)
{
  if (caller == NULL || cache_pool_id >= KMEM_MAX_POOLS || align == 0)
    return -1;

  pthread_mutex_lock(&kmem_lock);
  if (kmem_pools[cache_pool_id].used) {
    pthread_mutex_unlock(&kmem_lock);
    return -1;
  }
  addr_t addr = __kmem_carve((int)size);
  if (addr == 0) {
    pthread_mutex_unlock(&kmem_lock);
    return -1;
  }
  kmem_pools[cache_pool_id].used      = 1;
  kmem_pools[cache_pool_id].size      = (int)size;
  kmem_pools[cache_pool_id].align     = (int)align;
  kmem_pools[cache_pool_id].storage   = addr;
  kmem_pools[cache_pool_id].next_slot = 0;
  pthread_mutex_unlock(&kmem_lock);

#ifdef IODUMP
  printf("PID=%u kmem_cache_create pool=%u size=%u align=%u at 0x%lx\n",
         caller->pid, cache_pool_id, (unsigned)size, (unsigned)align,
         (unsigned long)addr);
#endif
  return 0;
}

/*libkmem_cache_alloc - allocate one slot from a cache pool
 *@caller: caller
 *@reg_index: symbol-table index that records the slot address
 *@cache_pool_id: source pool
 *
 * Argument order follows the loader/cpu dispatch: instruction
 * "kmem_cache_alloc [reg] [pool_id]" maps to (caller, reg_index, pool_id).
 */
int libkmem_cache_alloc(struct pcb_t *caller, uint32_t reg_index, uint32_t cache_pool_id)
{
  if (caller == NULL || caller->mm == NULL ||
      reg_index >= PAGING_MAX_SYMTBL_SZ || cache_pool_id >= KMEM_MAX_POOLS)
    return -1;

  pthread_mutex_lock(&kmem_lock);
  struct kmem_cache_pool_state *pool = &kmem_pools[cache_pool_id];
  if (!pool->used || pool->next_slot + pool->align > pool->size) {
    pthread_mutex_unlock(&kmem_lock);
    return -1;
  }
  addr_t slot_addr = pool->storage + (addr_t)pool->next_slot;
  int slot_size = pool->align;
  pool->next_slot += pool->align;
  pthread_mutex_unlock(&kmem_lock);

  caller->mm->symrgtbl[reg_index].rg_start = slot_addr;
  caller->mm->symrgtbl[reg_index].rg_end   = slot_addr + (addr_t)slot_size;
  caller->mm->symrgtbl[reg_index].rg_next  = NULL;

#ifdef IODUMP
  printf("PID=%u kmem_cache_alloc pool=%u region=%u at 0x%lx (size=%d)\n",
         caller->pid, cache_pool_id, reg_index,
         (unsigned long)slot_addr, slot_size);
#endif
  return 0;
}

/* Lower-level helpers retained for symmetry with the spec's libmem layout. */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  (void)vmaid;
  if (libkmem_malloc(caller, (uint32_t)size, (uint32_t)rgid) != 0)
    return 0;
  if (alloc_addr)
    *alloc_addr = caller->mm->symrgtbl[rgid].rg_start;
  return caller->mm->symrgtbl[rgid].rg_start;
}

addr_t __kmem_cache_alloc(struct pcb_t *caller, int vmaid, int rgid,
                          int cache_pool_id, addr_t *alloc_addr)
{
  (void)vmaid;
  if (libkmem_cache_alloc(caller, (uint32_t)rgid, (uint32_t)cache_pool_id) != 0)
    return 0;
  if (alloc_addr)
    *alloc_addr = caller->mm->symrgtbl[rgid].rg_start;
  return caller->mm->symrgtbl[rgid].rg_start;
}

/* Direct byte access into the simulated kernel storage. */
static int __kmem_write_byte(addr_t kaddr, BYTE value)
{
  if (!kmem_addr_is_kernel(kaddr))
    return -1;
  kmem_storage[__kmem_offset(kaddr)] = value;
  return 0;
}

static int __kmem_read_byte(addr_t kaddr, BYTE *value)
{
  if (!kmem_addr_is_kernel(kaddr))
    return -1;
  *value = kmem_storage[__kmem_offset(kaddr)];
  return 0;
}

/*libkmem_copy_from_user - move bytes from a user region into a kernel region
 *@caller: caller
 *@source: user-side symbol-table index
 *@destination: kernel-side symbol-table index (must hold a kmalloc'd region)
 *@offset: starting byte offset within each region
 *@size: number of bytes to copy
 */
int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source,
                           uint32_t destination, uint32_t offset, uint32_t size)
{
  if (caller == NULL || caller->mm == NULL)
    return -1;

  struct vm_rg_struct *src = get_symrg_byid(caller->mm, source);
  struct vm_rg_struct *dst = get_symrg_byid(caller->mm, destination);
  if (src == NULL || dst == NULL ||
      src->rg_start == src->rg_end || dst->rg_start == dst->rg_end)
    return -1;
  if (!kmem_addr_is_kernel(dst->rg_start))
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  for (uint32_t i = 0; i < size; i++) {
    BYTE b = 0;
    if (pg_getval(caller->mm, src->rg_start + offset + i, &b, caller) != 0) {
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }
    if (__kmem_write_byte(dst->rg_start + offset + i, b) != 0) {
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }
  }
  pthread_mutex_unlock(&mmvm_lock);

#ifdef IODUMP
  printf("PID=%u copy_from_user src_region=%u dst_region=%u offset=%u size=%u\n",
         caller->pid, source, destination, offset, size);
#endif
  return 0;
}

/*libkmem_copy_to_user - move bytes from a kernel region into a user region
 *@caller: caller
 *@source: kernel-side symbol-table index
 *@destination: user-side symbol-table index
 *@offset: starting byte offset within each region
 *@size: number of bytes to copy
 */
int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source,
                         uint32_t destination, uint32_t offset, uint32_t size)
{
  if (caller == NULL || caller->mm == NULL)
    return -1;

  struct vm_rg_struct *src = get_symrg_byid(caller->mm, source);
  struct vm_rg_struct *dst = get_symrg_byid(caller->mm, destination);
  if (src == NULL || dst == NULL ||
      src->rg_start == src->rg_end || dst->rg_start == dst->rg_end)
    return -1;
  if (!kmem_addr_is_kernel(src->rg_start))
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  for (uint32_t i = 0; i < size; i++) {
    BYTE b = 0;
    if (__kmem_read_byte(src->rg_start + offset + i, &b) != 0) {
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }
    if (pg_setval(caller->mm, dst->rg_start + offset + i, b, caller) != 0) {
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }
  }
  pthread_mutex_unlock(&mmvm_lock);

#ifdef IODUMP
  printf("PID=%u copy_to_user src_region=%u dst_region=%u offset=%u size=%u\n",
         caller->pid, source, destination, offset, size);
#endif
  return 0;
}

/*free_pcb_memphy - collect all memphy of pcb
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 */
int free_pcb_memph(struct pcb_t *caller)
{
  pthread_mutex_lock(&mmvm_lock);
  int pagenum, fpn;
  uint32_t pte;
  int max_pgn;

#ifdef MM64
  max_pgn = PAGING64_MAX_PGN;
#else
  max_pgn = PAGING_MAX_PGN;
#endif

  for (pagenum = 0; pagenum < max_pgn; pagenum++)
  {
#ifdef MM64
    pte = (uint32_t)caller->mm->pt[pagenum];
#else
    pte = caller->mm->pgd[pagenum];
#endif

    if (pte == 0)
      continue;

    if (PAGING_PAGE_PRESENT(pte) && !(pte & PAGING_PTE_SWAPPED_MASK))
    {
      fpn = PAGING_FPN(pte);
      MEMPHY_put_freefp(caller->krnl->mram, fpn);
    }
    else if (pte & PAGING_PTE_SWAPPED_MASK)
    {
      fpn = PAGING_SWP(pte);
      MEMPHY_put_freefp(caller->krnl->active_mswp, fpn);
    }
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}


/* Release per-process resources: physical frames (via free_pcb_memph),
 * page-table arrays, VMA list (with free-region list) and the code
 * segment. Finally frees the PCB itself. */
void free_pcb(struct pcb_t *proc)
{
  if (proc == NULL)
    return;

  if (proc->mm != NULL)
  {
    free_pcb_memph(proc);

    struct vm_area_struct *vma = proc->mm->mmap;
    while (vma != NULL)
    {
      struct vm_area_struct *next_vma = vma->vm_next;
      struct vm_rg_struct *rg = vma->vm_freerg_list;
      while (rg != NULL)
      {
        struct vm_rg_struct *next_rg = rg->rg_next;
        free(rg);
        rg = next_rg;
      }
      free(vma);
      vma = next_vma;
    }

    struct pgn_t *pg = proc->mm->fifo_pgn;
    while (pg != NULL)
    {
      struct pgn_t *next_pg = pg->pg_next;
      free(pg);
      pg = next_pg;
    }

#ifdef MM64
    free(proc->mm->pgd);
    free(proc->mm->p4d);
    free(proc->mm->pud);
    free(proc->mm->pmd);
    free(proc->mm->pt);
#else
    free(proc->mm->pgd);
#endif

    free(proc->mm);
  }

  if (proc->code != NULL)
  {
    free(proc->code->text);
    free(proc->code);
  }

  free(proc->page_table);
  free(proc);
}

/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  struct pgn_t *pg = mm->fifo_pgn;

  /* TODO: Implement the theorical mechanism to find the victim page */
  if (!pg)
  {
    return -1;
  }
  struct pgn_t *prev = NULL;
  while (pg->pg_next)
  {
    prev = pg;
    pg = pg->pg_next;
  }
  *retpgn = pg->pgn;
  if (prev != NULL)
    prev->pg_next = NULL;
  else
    mm->fifo_pgn = NULL;

  free(pg);

  return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;

  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start + size <= rgit->rg_end)
    { /* Current region has enough space */
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end)
      {
        rgit->rg_start = rgit->rg_start + size;
      }
      else
      { /*Use up all space, remove current node */
        /*Clone next rg node */
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        if (nextrg != NULL)
        {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;

          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        }
        else
        {                                /*End of free list */
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    }
    else
    {
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == -1) // new region not found
    return -1;

  return 0;
}

// #endif
