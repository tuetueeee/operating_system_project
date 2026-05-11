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
 * Memory physical module mm/mm-memphy.c
 */

#include "mm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 *  MEMPHY_mv_csr - move MEMPHY cursor
 *  @mp: memphy struct
 *  @offset: offset
 */
int MEMPHY_mv_csr(struct memphy_struct *mp, addr_t offset)
{
   int numstep = 0;

   mp->cursor = 0;
   while (numstep < offset && numstep < mp->maxsz)
   {
      /* Traverse sequentially */
      mp->cursor = (mp->cursor + 1) % mp->maxsz;
      numstep++;
   }

   return 0;
}

/*
 *  MEMPHY_seq_read - read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_seq_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL)
      return -1;

   if (!mp->rdmflg)
      return -1; /* Not compatible mode for sequential read */

   MEMPHY_mv_csr(mp, addr);
   *value = (BYTE)mp->storage[addr];

   return 0;
}

/*
 *  MEMPHY_read read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   int ret = 0;

   if (mp == NULL)
      return -1;

   pthread_mutex_lock(&mem_lock);
   if (mp->rdmflg)
      *value = mp->storage[addr];
   else /* Sequential access device */
      ret = MEMPHY_seq_read(mp, addr, value);
   pthread_mutex_unlock(&mem_lock);

   return ret;
}

/*
 *  MEMPHY_seq_write - write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_seq_write(struct memphy_struct *mp, addr_t addr, BYTE value)
{

   if (mp == NULL)
      return -1;

   if (!mp->rdmflg)
      return -1; /* Not compatible mode for sequential read */

   MEMPHY_mv_csr(mp, addr);
   mp->storage[addr] = value;

   return 0;
}

/*
 *  MEMPHY_write-write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_write(struct memphy_struct *mp, addr_t addr, BYTE data)
{
   int ret = 0;

   if (mp == NULL)
      return -1;

   pthread_mutex_lock(&mem_lock);
   if (mp->rdmflg)
      mp->storage[addr] = data;
   else /* Sequential access device */
      ret = MEMPHY_seq_write(mp, addr, data);
   pthread_mutex_unlock(&mem_lock);

   return ret;
}

/*
 *  MEMPHY_format-format MEMPHY device
 *  @mp: memphy struct
 */
int MEMPHY_format(struct memphy_struct *mp, int pagesz)
{
   /* This setting come with fixed constant PAGESZ */
   int numfp = mp->maxsz / pagesz;
   struct framephy_struct *newfst, *fst;
   int iter = 0;

   if (numfp <= 0)
      return -1;

   /* Init head of free framephy list */
   fst = malloc(sizeof(struct framephy_struct));
   fst->fpn = iter;
   mp->free_fp_list = fst;

   /* We have list with first element, fill in the rest num-1 element member*/
   for (iter = 1; iter < numfp; iter++)
   {
      newfst = malloc(sizeof(struct framephy_struct));
      newfst->fpn = iter;
      newfst->fp_next = NULL;
      fst->fp_next = newfst;
      fst = newfst;
   }

   return 0;
}

int MEMPHY_get_freefp(struct memphy_struct *mp, addr_t *retfpn)
{
   pthread_mutex_lock(&mem_lock);
   struct framephy_struct *fp = mp->free_fp_list;

   if (fp == NULL) {
      pthread_mutex_unlock(&mem_lock);
      return -1;
   }

   *retfpn = fp->fpn;
   mp->free_fp_list = fp->fp_next;

   /* MEMPHY is iteratively used up until its exhausted
    * No garbage collector acting then it not been released
    */
   free(fp);
   pthread_mutex_unlock(&mem_lock);

   return 0;
}

int MEMPHY_dump(struct memphy_struct *mp)
{
  /*TODO dump memphy contnt mp->storage
   *     for tracing the memory content
   */
   if (mp == NULL || mp->storage == NULL) {
        return -1;
    }

   printf("\n--- Physical Memory Dump (Size: %ld) ---\n", (long)mp->maxsz);
    
   int has_data = 0;
   for (addr_t addr = 0; addr < (addr_t)mp->maxsz; addr++) {
        /* Chỉ in các ô nhớ có giá trị khác 0 để dễ theo dõi */
        if (mp->storage[addr] != 0) {
            printf("[0x%08llx]: 0x%02x\n",
                   (unsigned long long)addr, mp->storage[addr]);
            has_data = 1;
        }
    }

   if (!has_data) {
      printf("(Physical memory is currently empty/all zeros)\n");
    }
    
   printf("--- End of Dump ---\n\n");
   return 0;
}

int MEMPHY_put_freefp(struct memphy_struct *mp, addr_t fpn)
{
   pthread_mutex_lock(&mem_lock);
   struct framephy_struct *fp = mp->free_fp_list;
   struct framephy_struct *newnode = malloc(sizeof(struct framephy_struct));

   /* Create new node with value fpn */
   newnode->fpn = fpn;
   newnode->fp_next = fp;
   mp->free_fp_list = newnode;
   pthread_mutex_unlock(&mem_lock);
   return 0;
}

/*
 *  Init MEMPHY struct
 */
int init_memphy(struct memphy_struct *mp, addr_t max_size, int randomflg)
{
   mp->storage = (BYTE *)malloc(max_size * sizeof(BYTE));
   mp->maxsz = max_size;
   memset(mp->storage, 0, max_size * sizeof(BYTE));

   mp->free_fp_list = NULL;
   mp->used_fp_list = NULL;

   MEMPHY_format(mp, PAGING_PAGESZ);

   mp->rdmflg = (randomflg != 0) ? 1 : 0;

   if (!mp->rdmflg) /* Not Random acess device, then it is a serial device */
      mp->cursor = 0;

   return 0;
}

/* Release all memory owned by a MEMPHY: the byte buffer and the
 * free/used frame-tracking linked lists. */
void finish_memphy(struct memphy_struct *mp)
{
   if (mp == NULL)
      return;

   struct framephy_struct *fp = mp->free_fp_list;
   while (fp != NULL) {
      struct framephy_struct *next = fp->fp_next;
      free(fp);
      fp = next;
   }
   mp->free_fp_list = NULL;

   fp = mp->used_fp_list;
   while (fp != NULL) {
      struct framephy_struct *next = fp->fp_next;
      free(fp);
      fp = next;
   }
   mp->used_fp_list = NULL;

   free(mp->storage);
   mp->storage = NULL;
}

// #endif
