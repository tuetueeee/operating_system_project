/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

/*
 * PAGING based Memory Management
 * Memory management unit mm/mm.c
 */

#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(MM64)

/*
 * get_pd_from_address - Parse address to 5 page directory level
 * @addr  : address
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table
 */
int get_pd_from_address(addr_t addr, addr_t *pgd, addr_t *p4d, addr_t *pud, addr_t *pmd, addr_t *pt)
{
  /* Extract page direactories */
  *pgd = (addr & PAGING64_ADDR_PGD_MASK) >> PAGING64_ADDR_PGD_LOBIT;
  *p4d = (addr & PAGING64_ADDR_P4D_MASK) >> PAGING64_ADDR_P4D_LOBIT;
  *pud = (addr & PAGING64_ADDR_PUD_MASK) >> PAGING64_ADDR_PUD_LOBIT;
  *pmd = (addr & PAGING64_ADDR_PMD_MASK) >> PAGING64_ADDR_PMD_LOBIT;
  *pt = (addr & PAGING64_ADDR_PT_MASK) >> PAGING64_ADDR_PT_LOBIT;

  return 0;
}

/*
 * get_pd_from_pagenum - Parse page number to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table
 */
int get_pd_from_pagenum(addr_t pgn, addr_t *pgd, addr_t *p4d, addr_t *pud, addr_t *pmd, addr_t *pt)
{
  /* Shift the address to get page num and perform the mapping*/
  return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
                             pgd, p4d, pud, pmd, pt);
}

/* Real 5-level paging (demand-allocated, sparse).
 * mm->pgd is the only directory allocated up front. P4D/PUD/PMD/PT are
 * created on first use via pte_walk(alloc=1). Non-leaf entries hold a
 * pointer to the child array cast through uintptr_t; leaf PT entries
 * hold the actual PTE value (FPN/SWAP/flags).
 */

static inline addr_t *dir_load(addr_t *parent, addr_t idx)
{
  if (parent == NULL)
    return NULL;
  return (addr_t *)(uintptr_t)parent[idx];
}

/* Allocate a child directory on demand. `stats_mm` may be NULL when no
 * statistics tracking is wanted; otherwise its counters are updated. */
static addr_t *dir_load_or_alloc(addr_t *parent, addr_t idx,
                                 struct mm_struct *stats_mm)
{
  addr_t *child = dir_load(parent, idx);
  if (child != NULL)
    return child;
  child = calloc(PAGING64_DIR_ENTRIES, sizeof(addr_t));
  if (child == NULL)
    return NULL;
  parent[idx] = (addr_t)(uintptr_t)child;
  if (stats_mm != NULL) {
    stats_mm->mm64_dir_alloc_count++;
    stats_mm->mm64_bytes_alloc +=
        (uint64_t)PAGING64_DIR_ENTRIES * sizeof(addr_t);
  }
  return child;
}

/* pte_walk - locate the PTE slot for `pgn`. If `alloc` is non-zero, every
 * missing intermediate directory is created on demand; otherwise returns
 * NULL when any level is absent. */
static addr_t *pte_walk(struct mm_struct *mm, addr_t pgn, int alloc)
{
  /* Use the simulator-wide page-count limit (PAGING_MAX_PGN in mm.h),
   * not PAGING64_MAX_PGN. The latter assumes 4KB pages and silently
   * rejects pgn >= 512, but pgn here is computed against PAGING_PAGESZ
   * (256B) so legitimate addresses can reach up to PAGING_MAX_PGN. */
  if (mm == NULL || mm->pgd == NULL || pgn >= PAGING_MAX_PGN)
    return NULL;

  addr_t pgd_i = 0, p4d_i = 0, pud_i = 0, pmd_i = 0, pt_i = 0;
  get_pd_from_pagenum(pgn, &pgd_i, &p4d_i, &pud_i, &pmd_i, &pt_i);

  /* Statistics: every successful indirection counts as one memory access,
   * the final PTE read/write is the fifth. We tally per level traversed
   * so a walk that bails out partway still contributes truthfully. */
  mm->mm64_walk_count++;

  struct mm_struct *sm = alloc ? mm : NULL;  /* only mutate stats when allocating */

  mm->mm64_mem_access_count++;               /* PGD entry */
  addr_t *p4d = alloc ? dir_load_or_alloc(mm->pgd, pgd_i, sm) : dir_load(mm->pgd, pgd_i);
  if (p4d == NULL) return NULL;
  mm->mm64_mem_access_count++;               /* P4D entry */
  addr_t *pud = alloc ? dir_load_or_alloc(p4d, p4d_i, sm) : dir_load(p4d, p4d_i);
  if (pud == NULL) return NULL;
  mm->mm64_mem_access_count++;               /* PUD entry */
  addr_t *pmd = alloc ? dir_load_or_alloc(pud, pud_i, sm) : dir_load(pud, pud_i);
  if (pmd == NULL) return NULL;
  mm->mm64_mem_access_count++;               /* PMD entry */
  addr_t *pt  = alloc ? dir_load_or_alloc(pmd, pmd_i, sm) : dir_load(pmd, pmd_i);
  if (pt  == NULL) return NULL;
  mm->mm64_mem_access_count++;               /* PT entry (the actual PTE) */

  return &pt[pt_i];
}

/*
 * pte_set_swap - Set PTE entry for swapped page
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  addr_t *pte = pte_walk(caller->mm, pgn, /*alloc=*/1);
  if (pte == NULL)
    return -1;

  /* The page is no longer present in RAM, only in swap. */
  CLRBIT(*pte, PAGING_PTE_PRESENT_MASK);
  CLRBIT(*pte, PAGING_PTE_FPN_MASK);
  SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
  SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);

  return 0;
}

/*
 * pte_set_fpn - Set PTE entry for on-line page
 */
int pte_set_fpn(struct pcb_t *caller, addr_t pgn, addr_t fpn)
{
  addr_t *pte = pte_walk(caller->mm, pgn, /*alloc=*/1);
  if (pte == NULL)
    return -1;

  SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
  CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);
  SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);

  return 0;
}

/* Get PTE: returns 0 (the "absent" PTE) if any level along the walk is
 * unallocated. */
uint32_t pte_get_entry(struct pcb_t *caller, addr_t pgn)
{
  addr_t *pte = pte_walk(caller->mm, pgn, /*alloc=*/0);
  if (pte == NULL)
    return 0;
  return (uint32_t)*pte;
}

int pte_set_entry(struct pcb_t *caller, addr_t pgn, uint32_t pte_val)
{
  addr_t *pte = pte_walk(caller->mm, pgn, /*alloc=*/1);
  if (pte == NULL)
    return -1;
  *pte = pte_val;
  return 0;
}

/*
 * vmap_pgd_memset - map a range of page at aligned address (dummy allocation
 * for the 64-bit large-address scheme: emulate page-directory behavior with
 * a recognizable marker pattern but *no* real frame backing).
 *
 * The pattern must NOT have the PRESENT bit (31) or SWAPPED bit (30) set:
 * free_pcb_memph walks every PTE on exit and would otherwise interpret the
 * marker as a real FPN/SWP-slot and return a phantom frame to MEMPHY's
 * free list, corrupting the free-frame pool.
 */
int vmap_pgd_memset(struct pcb_t *caller, // process call
                    addr_t addr,          // start address which is aligned to pagesz
                    int pgnum)            // num of mapping page
{
  /* Marker pattern: bit 29 (RESERVED) set, low bits spell "DEAD".
   * PRESENT(31)=0 and SWAPPED(30)=0 → free_pcb_memph skips these PTEs. */
  uint32_t pattern = PAGING_PTE_RESERVE_MASK | 0x0000DEAD;
  addr_t pgn_start = addr / PAGING_PAGESZ;

  for (int pgit = 0; pgit < pgnum; pgit++) {
    if (pte_set_entry(caller, pgn_start + pgit, pattern) != 0)
      return -1;
  }

  return 0;
}

/*
 * vmap_page_range - map a range of page at aligned address
 */
addr_t vmap_page_range(struct pcb_t *caller,           // process call
                       addr_t addr,                    // start address which is aligned to pagesz
                       int pgnum,                      // num of mapping page
                       struct framephy_struct *frames, // list of the mapped frames
                       struct vm_rg_struct *ret_rg)    // return mapped region, the real mapped fp
{                                                      // no guarantee all given pages are mapped
  struct framephy_struct *fpit = frames;
  addr_t pgn_start = addr / PAGING_PAGESZ;

  /* Update the rg_end and rg_start of ret_rg */
  ret_rg->rg_start = addr;
  ret_rg->rg_end = addr + (pgnum * PAGING_PAGESZ);

  /* Map range of frame to address space. The framephy_struct nodes are
   * consumed here — once the FPN is committed to the page table we
   * release the node so the caller doesn't need to walk the list again.
   *
   * If pte_set_fpn fails (e.g. directory calloc returned NULL deep inside
   * pte_walk, or pgn exceeds PAGING_MAX_PGN), the frame must go back to
   * MEMRAM's free pool — otherwise the FPN is silently leaked since the
   * framephy node holding it is about to be freed too. We also skip the
   * FIFO enlist so find_victim_page won't later pick an unmapped pgn.
   */
  for (int pgit = 0; pgit < pgnum && fpit != NULL; pgit++) {
    addr_t pgn = pgn_start + pgit;
    if (pte_set_fpn(caller, pgn, fpit->fpn) == 0) {
      enlist_pgn_node(&caller->mm->fifo_pgn, pgn);
    } else {
      MEMPHY_put_freefp(caller->krnl->mram, fpit->fpn);
    }

    struct framephy_struct *next = fpit->fp_next;
    free(fpit);
    fpit = next;
  }

  /* Free any trailing frames that weren't mapped (shouldn't normally
   * happen, but be defensive). */
  while (fpit != NULL) {
    struct framephy_struct *next = fpit->fp_next;
    MEMPHY_put_freefp(caller->krnl->mram, fpit->fpn);
    free(fpit);
    fpit = next;
  }

  return 0;
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller
 * @req_pgnum : request page num
 * @frm_lst   : frame list
 */
addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
  addr_t fpn;
  struct framephy_struct *head = NULL;
  struct framephy_struct *tail = NULL;

  for (int pgit = 0; pgit < req_pgnum; pgit++)
  {
    if (MEMPHY_get_freefp(caller->krnl->mram, &fpn) != 0)
    {
      /* Out of RAM: roll the partially-allocated chain back. Returning
       * a half list would leak the frames into the caller's PTEs but
       * leave the request as a whole un-fulfilled.
       */
      while (head != NULL)
      {
        struct framephy_struct *next = head->fp_next;
        MEMPHY_put_freefp(caller->krnl->mram, head->fpn);
        free(head);
        head = next;
      }
      *frm_lst = NULL;
      return -1;
    }

    struct framephy_struct *newfp_str = malloc(sizeof(struct framephy_struct));
    newfp_str->fpn = fpn;
    newfp_str->fp_next = NULL;

    if (head == NULL)
      head = tail = newfp_str;
    else {
      tail->fp_next = newfp_str;
      tail = newfp_str;
    }
  }
  *frm_lst = head;

  return 0;
}

/*
 * vm_map_range - do the mapping all vm area to ram storage device
 * @caller    : caller
 * @astart    : vm area start
 * @aend      : vm area end
 * @mapstart  : start mapping point
 * @incpgnum  : number of mapped page
 * @ret_rg    : returned region
 */
addr_t vm_map_range(struct pcb_t *caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  (void)astart;
  (void)aend;

  struct framephy_struct *frm_lst = NULL;

  /* alloc_pages_range is all-or-nothing: on success it returns 0 with
   * a chain of incpgnum frames; on failure it releases the partial
   * allocation and returns (addr_t)-1. The simulator does not implement
   * on-demand swap-out for fresh allocations, so callers are expected
   * to size their requests to fit within free RAM.
   */
  if (alloc_pages_range(caller, incpgnum, &frm_lst) != 0)
    return -1;

  vmap_page_range(caller, mapstart, incpgnum, frm_lst, ret_rg);
  return 0;
}

/* Swap copy content page from source frame to destination frame
 * @mpsrc  : source memphy
 * @srcfpn : source physical page number (FPN)
 * @mpdst  : destination memphy
 * @dstfpn : destination physical page number (FPN)
 **/
int __swap_cp_page(struct memphy_struct *mpsrc, addr_t srcfpn,
                   struct memphy_struct *mpdst, addr_t dstfpn)
{
  int cellidx;
  addr_t addrsrc, addrdst;
  for (cellidx = 0; cellidx < PAGING_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING_PAGESZ + cellidx;

    BYTE data;
    MEMPHY_read(mpsrc, addrsrc, &data);
    MEMPHY_write(mpdst, addrdst, data);
  }

  return 0;
}

/* Recursive teardown for the demand-allocated directory tree. `level` is
 * the number of indirections remaining beneath `dir`: 5 = PGD, 4 = P4D,
 * 3 = PUD, 2 = PMD, 1 = PT. Leaf PT entries hold PTE values (no further
 * allocation), so we only recurse while level > 1. */
static void mm64_free_dir(addr_t *dir, int level)
{
  if (dir == NULL)
    return;
  if (level > 1) {
    for (unsigned i = 0; i < PAGING64_DIR_ENTRIES; i++) {
      if (dir[i] != 0)
        mm64_free_dir((addr_t *)(uintptr_t)dir[i], level - 1);
    }
  }
  free(dir);
}

void mm64_destroy_pgd_tree(struct mm_struct *mm)
{
  if (mm == NULL || mm->pgd == NULL)
    return;
  mm64_free_dir(mm->pgd, 5);
  mm->pgd = NULL;
}

/*
 * init_mm - Initialize a empty Memory Management instance
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  struct vm_area_struct *vma0 = malloc(sizeof(struct vm_area_struct));

  /* Only the root directory is allocated up front. Lower levels are
   * created on first access by pte_walk(); a process that never touches
   * a given address range pays nothing for the corresponding sub-trees.
   */
#ifdef MM64
  mm->pgd = calloc(PAGING64_DIR_ENTRIES, sizeof(addr_t));
  mm->p4d = NULL;
  mm->pud = NULL;
  mm->pmd = NULL;
  mm->pt  = NULL;
  /* Initialise the per-mm 5-level paging statistics. PGD itself counts
   * as one directory page (always present, never lazy). */
  mm->mm64_walk_count        = 0;
  mm->mm64_mem_access_count  = 0;
  mm->mm64_dir_alloc_count   = 1;
  mm->mm64_bytes_alloc       = (uint64_t)PAGING64_DIR_ENTRIES * sizeof(addr_t);
#else
  mm->pgd = calloc(PAGING_MAX_PGN, sizeof(uint32_t));
#endif

  /* By default the owner comes with at least one vma */
  vma0->vm_id = 0;
  vma0->vm_start = 0;
  vma0->vm_end = vma0->vm_start;
  vma0->sbrk = vma0->vm_start;
  struct vm_rg_struct *first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

  vma0->vm_next = NULL;

  /* Point vma owner backward */
  vma0->vm_mm = mm;

  /* Update mmap and other mm fields */
  mm->mmap = vma0;
  mm->fifo_pgn = NULL;
  mm->kcpooltbl = NULL;

  /* mm is malloc'd in os.c without zeroing — explicitly clear the symbol
   * table so __free's "(rg_start == 0 && rg_end == 0) means unallocated"
   * check is reliable. */
  memset(mm->symrgtbl, 0, sizeof(mm->symrgtbl));

  return 0;
}

struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end)
{
  struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));

  rgnode->rg_start = rg_start;
  rgnode->rg_end = rg_end;
  rgnode->rg_next = NULL;

  return rgnode;
}

int enlist_vm_rg_node(struct vm_rg_struct **rglist, struct vm_rg_struct *rgnode)
{
  rgnode->rg_next = *rglist;
  *rglist = rgnode;

  return 0;
}

int enlist_pgn_node(struct pgn_t **plist, addr_t pgn)
{
  struct pgn_t *pnode = malloc(sizeof(struct pgn_t));

  pnode->pgn = pgn;
  pnode->pg_next = *plist;
  *plist = pnode;

  return 0;
}

int print_pgtbl(struct pcb_t *caller, addr_t start, addr_t end)
{
  /* Clamp end to the simulator's page count and index by PAGING_PAGESZ
   * (the actual frame size used by the rest of the paging path). */
  addr_t max_pgn = PAGING_MAX_PGN - 1;
  addr_t pgn_start = start / PAGING_PAGESZ;
  addr_t pgn_end = (end == (addr_t)-1 || end / PAGING_PAGESZ > max_pgn)
                   ? max_pgn : end / PAGING_PAGESZ;
  addr_t pgit;

  addr_t pgd = 0;
  addr_t p4d = 0;
  addr_t pud = 0;
  addr_t pmd = 0;
  addr_t pt = 0;

  get_pd_from_address(start, &pgd, &p4d, &pud, &pmd, &pt);

  printf("print_pgtbl: PID=%u\n", caller->pid);
  printf(" PGD[%lu] P4D[%lu] PUD[%lu] PMD[%lu] PT[%lu]\n",
         (unsigned long)pgd, (unsigned long)p4d, (unsigned long)pud,
         (unsigned long)pmd, (unsigned long)pt);
  for (pgit = pgn_start; pgit <= pgn_end; pgit++) {
    uint32_t pte = pte_get_entry(caller, pgit);
    if (pte != 0) {
      printf("  Page %lu -> PTE 0x%08x\n", (unsigned long)pgit, pte);
    }
  }

  return 0;
}

#endif // def MM64
