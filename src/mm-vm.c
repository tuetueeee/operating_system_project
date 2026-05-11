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
 * PAGING based Memory Management
 * Virtual memory module mm/mm-vm.c
 */

#include "string.h"
#include "mm.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

/*get_vma_by_num - get vm area by numID
 *@mm: memory region
 *@vmaid: ID vm area to alloc memory region
 *
 */
struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  struct vm_area_struct *pvma = mm->mmap;

  if (mm->mmap == NULL)
    return NULL;

  int vmait = pvma->vm_id;

  while (vmait < vmaid)
  {
    if (pvma == NULL)
      return NULL;

    pvma = pvma->vm_next;
    vmait = pvma->vm_id;
  }

  return pvma;
}

int __mm_swap_page(struct pcb_t *caller, addr_t vicfpn, addr_t swpfpn)
{
  __swap_cp_page(caller->krnl->mram, vicfpn, caller->krnl->active_mswp, swpfpn);
  return 0;
}

/*get_vm_area_node_at_brk - get vm area for a number of pages
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: requested size
 *@alignedsz: page aligned size
 *
 */
struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid, addr_t size, addr_t alignedsz)
{
  struct vm_rg_struct *newrg;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (cur_vma == NULL)
    return NULL;

  newrg = malloc(sizeof(struct vm_rg_struct));
  newrg->vmaid = vmaid;
  newrg->rg_start = cur_vma->sbrk;

  /* Sử dụng alignedsz thay vì size để đảm bảo căn lề trang */
  newrg->rg_end = newrg->rg_start + alignedsz;
  newrg->rg_next = NULL;

  return newrg;
}

/*validate_overlap_vm_area
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@vmastart: vma start
 *@vmaend: vma end
 *
 */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  if (vmastart >= vmaend)
  {
    return -1;
  }

  struct vm_area_struct *vma = caller->mm->mmap;
  if (vma == NULL)
  {
    return -1;
  }

  struct vm_area_struct *cur_area = get_vma_by_num(caller->mm, vmaid);
  if (cur_area == NULL)
  {
    return -1;
  }

  /* Duyệt qua toàn bộ danh sách VMA để kiểm tra xem vùng [vmastart, vmaend] có đè lên vma nào khác không */
  while (vma != NULL)
  {
    if (vma != cur_area)
    {
      /* Công thức chuẩn: hai đoạn [start1, end1] và [start2, end2] giao nhau khi (start1 < end2) && (end1 > start2) */
      if ((vmastart < vma->vm_end) && (vmaend > vma->vm_start))
      {
        return -1;
      }
    }
    vma = vma->vm_next;
  }

  return 0;
}

/*inc_vma_limit - increase vm area limits to reserve space for new variable
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@inc_sz: increment size
 *
 */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (cur_vma == NULL)
    return -1;

  /* Page-align the requested increment. */
  int incnumpage = (inc_sz + PAGING_PAGESZ - 1) / PAGING_PAGESZ;
  addr_t inc_amt = (addr_t)incnumpage * PAGING_PAGESZ;

  addr_t old_end = cur_vma->sbrk;
  addr_t old_vmend = cur_vma->vm_end;
  addr_t new_end = old_end + inc_amt;

  if (validate_overlap_vm_area(caller, vmaid, old_end, new_end) < 0)
    return -1;

  cur_vma->sbrk = new_end;
  if (cur_vma->sbrk > cur_vma->vm_end)
    cur_vma->vm_end = cur_vma->sbrk;

  /* The mapped-region descriptor is consumed only by the caller's
   * bookkeeping; keep it on the stack so we don't have to free it.
   */
  struct vm_rg_struct mapped_rg = { 0 };
  if (vm_map_range(caller, cur_vma->vm_start, cur_vma->vm_end,
                   old_end, incnumpage, &mapped_rg) < 0)
  {
    /* Restore VMA limits so subsequent allocations see consistent state. */
    cur_vma->sbrk = old_end;
    cur_vma->vm_end = old_vmend;
    return -1;
  }

  return 0;
}

// #endif