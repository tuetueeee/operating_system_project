# Simple Operating System — HCMUT CO2018 Assignment

A simulator of a small operating system kernel covering the three core
subsystems taught in the OS course: **scheduling**, **paging-based
memory management with separable user/kernel space**, and a **system
call interface**. Built around the *Caitoa* release of the assignment
skeleton.

## Table of contents

- [Highlights](#highlights)
- [Build & run](#build--run)
- [Repository layout](#repository-layout)
- [Architecture](#architecture)
  - [Scheduler — multi-level queue (MLQ)](#scheduler--multi-level-queue-mlq)
  - [Virtual memory & paging](#virtual-memory--paging)
  - [64-bit multi-level paging](#64-bit-multi-level-paging)
  - [System call layer](#system-call-layer)
  - [Kernel-memory subsystem (kmem)](#kernel-memory-subsystem-kmem)
- [Process programs](#process-programs)
- [Configuration files](#configuration-files)
- [Compile-time switches](#compile-time-switches)
- [Tests](#tests)
- [Synchronisation model](#synchronisation-model)
- [Memory hygiene](#memory-hygiene)
- [Known limitations](#known-limitations)
- [References](#references)

## Highlights

- **MLQ scheduler** with `MAX_PRIO = 140` priority queues and the
  `slot = MAX_PRIO − prio` traversal policy described in §2.1 of the
  spec.
- **Per-process paging** with a single VMA, FIFO page replacement, swap
  in/out via the syscall interface, and the simplified-flat 5-level
  page directory model (PGD/P4D/PUD/PMD/PT) for the 64-bit address
  scheme.
- **User/kernel space separation**: every syscall handler resolves the
  caller's PCB through the kernel `running_list` keyed by PID, instead
  of receiving a PCB pointer.
- **Kernel-memory subsystem** providing `kmalloc`, `kmem_cache_create`,
  `kmem_cache_alloc`, `copy_from_user` and `copy_to_user` against a
  bump-allocated kernel pool with slab caches.
- **Zero build warnings, zero runtime leaks** under macOS `leaks` on
  the bundled workloads.

## Build & run

```bash
make all              # produces ./os
./os <config_name>    # config_name is a file under input/
```

A typical session:

```bash
./os os_1_mlq_paging        # exercises scheduler + paging
./os os_kmem                # exercises kmem ops end-to-end
./os os_syscall_list        # invokes the listsyscall syscall
```

Reset build artefacts:

```bash
make clean
```

Re-run the bundled regression suite (writes outputs back to
`input/*.output` and then moves them to `output/`):

```bash
./run.sh
```

> The repository compiles cleanly under macOS (Clang) and Linux (GCC).
> No external dependencies beyond `pthread`.

## Repository layout

```
.
├── Makefile                Build rules
├── run.sh                  Convenience: run every config in input/
├── include/                Public headers
│   ├── common.h            PCB, kernel struct, instruction opcodes
│   ├── os-cfg.h            Compile-time switches
│   ├── os-mm.h             VMA, region, frame, mm_struct
│   ├── mm.h                Paging constants, prototypes
│   ├── mm64.h              64-bit address layout (PGD..PT bit fields)
│   ├── libmem.h            User-facing memory library prototypes
│   ├── syscall.h           Syscall register struct & dispatcher
│   ├── sched.h, queue.h    Scheduler & priority queue
│   ├── cpu.h, loader.h     CPU dispatcher + program loader
│   ├── timer.h, mem.h      Timer + legacy memory module
│   └── bitops.h            Bit-manipulation macros
├── src/
│   ├── os.c                main(); spawns loader and CPU threads
│   ├── sched.c, queue.c    MLQ scheduler and priority queue
│   ├── cpu.c               Per-instruction dispatcher
│   ├── loader.c            Reads the assembly-like program file
│   ├── timer.c             Time-slot synchronisation primitive
│   ├── libmem.c            liballoc/libfree/libread/libwrite,
│   │                       libkmem_* and __resolve_rg_addr helper
│   ├── mm.c, mm64.c        Page table / PTE helpers (32 & 64 bit)
│   ├── mm-vm.c             VMA growth (inc_vma_limit, swap helper)
│   ├── mm-memphy.c         Physical memory + frame free-list
│   ├── libstd.c            libsyscall wrapper
│   ├── syscall.c, sys_*.c  System-call table and handlers
│   └── mem.c               Legacy / obsolete non-paging memory
├── input/                  Configuration files + process programs
│   ├── proc/               Per-process program files
│   ├── os_*                Top-level OS configurations
│   └── sched_*             Scheduler-only configurations
└── output/                 Reference / sample outputs
```

## Architecture

```
            ┌───────────────────────────────┐
            │  Process programs (input/proc) │
            └──────────────┬────────────────┘
                           │ loader.c
                           ▼
        ┌──────────────────────────────────────┐
        │           Scheduler  (sched.c)        │
        │  MLQ ready_queues × MAX_PRIO          │
        └────────────┬───────────────┬──────────┘
                     │ get_proc       │ put_proc
                     ▼                ▲
    ┌─────────────────────────────────────────────┐
    │                  CPU threads (cpu.c)        │
    │  decodes instructions → liballoc/libread/…  │
    └──┬──────────────────────────────────────────┘
       │ user space
       │ libsyscall  ──►  syscall_tbl ──► sys_memmap, sys_listsyscall
       ▼ kernel space
    ┌─────────────────────────────────────────────┐
    │ libmem / mm-vm / mm64 / mm-memphy           │
    │   page table, MMU, MEMRAM, MEMSWP           │
    └─────────────────────────────────────────────┘
```

### Scheduler — multi-level queue (MLQ)

- `MAX_PRIO = 140` ready queues, one per priority level.
- `slot[prio] = MAX_PRIO − prio`. The scheduler picks the lowest-prio
  queue with `slot > 0` and a non-empty queue, decrements its slot,
  and round-robins among entries within that queue. When every queue
  exhausts its slot budget, slots are reset and traversal continues.
- The legacy `processed_queue` / `run_queue` is kept for source
  compatibility but is no longer part of the scheduling decision.
- Implemented in `src/sched.c` (`get_mlq_proc`, `put_mlq_proc`) on top
  of the simple priority queue in `src/queue.c`.

### Virtual memory & paging

Each process owns its own `mm_struct` containing:

- a singly-linked `vm_area_struct *mmap` list (currently a single VMA),
- a symbol table `symrgtbl[PAGING_MAX_SYMTBL_SZ]` mapping a region ID
  to `[rg_start, rg_end)`,
- a flat `pt[]` array of PTEs,
- a `fifo_pgn` list driving the page-replacement victim selection.

User code goes through `libmem.c`:

| Instruction | Library entry | Kernel side (syscall) |
|---|---|---|
| `alloc <size> <reg>` | `liballoc` → `__alloc` | `SYSMEM_INC_OP` → `inc_vma_limit` |
| `free <reg>` | `libfree` → `__free` | — |
| `read <src> <off> <dst>` | `libread` → `__read` | `SYSMEM_IO_READ` → `MEMPHY_read` |
| `write <data> <dst> <off>` | `libwrite` → `__write` | `SYSMEM_IO_WRITE` → `MEMPHY_write` |

`pg_getval` / `pg_setval` resolve a user virtual address to a frame:

```
addr  ─►  pgn = addr / PAGING_PAGESZ          (page lookup)
          off = addr % PAGING_PAGESZ
          pg_getpage(pgn) ─► fpn              (swap-in if needed)
          phyaddr = fpn × PAGING_PAGESZ + off
```

Page replacement is FIFO via `find_victim_page`. On a miss, the victim
frame is evicted to the active swap (`SYSMEM_SWP_OP`), the target page
is read back from swap into the freed frame, and both PTEs are updated.

### 64-bit multi-level paging

The address layout splits a 64-bit virtual address into:

```
| 63‒57  | 56‒48 | 47‒39 | 38‒30 | 29‒21 | 20‒12 | 11‒0   |
| unused |  PGD  |  P4D  |  PUD  |  PMD  |  PT   | OFFSET |
```

`get_pd_from_address` / `get_pd_from_pagenum` (in `src/mm64.c`)
compute the per-level indices. The simulator keeps a *flat* `pt[]`
array as the true mapping store; cascading directory entries
(`pgd[]`, `p4d[]`, `pud[]`, `pmd[]`) are populated with the
lower-level table pointer so `print_pgtbl` can display the
translation path. `vmap_pgd_memset` provides the dummy
allocation requested in spec §3.2.

### System call layer

`src/syscall.tbl` declares the syscall numbers:

```
0   listsyscall  sys_listsyscall
17  memmap       sys_memmap
```

`syscalltbl.sh` generates `src/syscalltbl.lst` at build time. The
dispatcher `_syscall(krnl, pid, nr, regs)` includes that file twice to
build both the extern declarations and the `switch` table.

**`sys_memmap`** routes through five sub-operations:

| `regs.a1` | Constant | Handler |
|---|---|---|
| 1 | `SYSMEM_MAP_OP` | `vmap_pgd_memset` (dummy alloc) |
| 2 | `SYSMEM_INC_OP` | `inc_vma_limit` (grow VMA) |
| 3 | `SYSMEM_SWP_OP` | `__mm_swap_page` (RAM ↔ swap copy) |
| 4 | `SYSMEM_IO_READ` | `MEMPHY_read` |
| 5 | `SYSMEM_IO_WRITE` | `MEMPHY_write` |

In line with spec §3.2 — *"Direct access from user space through a
process PCB is not allowed; only `struct krnl_t` may be used"* — each
handler **resolves the caller's PCB by walking
`krnl->running_list` to match `pid`** instead of trusting a PCB
pointer.

### Kernel-memory subsystem (kmem)

Implemented inside `src/libmem.c`. Backed by a 64KB static byte array
(`kmem_storage[]`) and a bump pointer (`kmem_brk`). Kernel virtual
addresses are encoded as `KMEM_VBASE | offset` with
`KMEM_VBASE = 1ULL << 56` so that `libfree` and validation routines
can tell them apart from user-space regions.

| Instruction | Behaviour |
|---|---|
| `kmalloc <size> <reg>` | Bump-allocate `size` bytes; record in `symrgtbl[reg]`. |
| `kmem_cache_create <size> <align> <pool>` | Reserve a slab of `size` bytes; each future slot is `align` bytes. |
| `kmem_cache_alloc <reg> <pool>` | Bump the slot pointer of `pool`; record in `symrgtbl[reg]`. |
| `copy_from_user <src> <dst> <off> <n>` | Read `n` bytes from user region `src+off` via the page table, write to kernel region `dst+off` directly. |
| `copy_to_user <src> <dst> <off> <n>` | Read `n` bytes from kernel region `src+off` directly, write to user region `dst+off` via the page table. |

`libfree` checks `kmem_addr_is_kernel(rg_start)` and clears the symbol
entry without enlisting it back into the per-VMA free list (the bump
allocator never reclaims; that is sufficient for the simulator's
short-lived workloads).

## Process programs

Each process is a plain text file under `input/proc/`. Header:

```
<priority> <N>
<instruction 1>
<instruction 2>
...
<instruction N>
```

Supported instructions:

| Mnemonic | Syntax | Description |
|---|---|---|
| `calc` | `calc` | No-op CPU work. |
| `alloc` | `alloc <size> <reg>` | User-space allocation. |
| `free` | `free <reg>` | Free the region recorded in `<reg>`. |
| `read` | `read <src> <off> <dst>` | `regs[dst] = mem[regs[src] + off]`. |
| `write` | `write <data> <dst> <off>` | `mem[regs[dst] + off] = data`. |
| `kmalloc` | `kmalloc <size> <reg>` | Kernel-space allocation. |
| `kmem_cache_create` | `kmem_cache_create <size> <align> <pool>` | Create a slab cache. |
| `kmem_cache_alloc` | `kmem_cache_alloc <reg> <pool>` | Allocate one slot. |
| `copy_from_user` | `copy_from_user <src> <dst> <off> <n>` | User → kernel copy. |
| `copy_to_user` | `copy_to_user <src> <dst> <off> <n>` | Kernel → user copy. |
| `syscall` | `syscall <nr> <a1> <a2> <a3>` | Direct syscall invocation. |

Sample (`input/proc/m2s`):

```
1 12
alloc 300 0
alloc 100 1
free 0
kmalloc 200 2
kmem_cache_create 300 10 1
kmem_cache_alloc 3 1
copy_from_user 1 4 0 3
kmalloc 200 4
copy_to_user 3 4 0 3
free 2
free 1
free 3
```

## Configuration files

Top-level OS configs live in `input/`. Two formats are supported:

**Modern format** (`MM_FIXED_MEMSZ` disabled):

```
<time_slice> <num_cpus> <num_processes>
<ram_sz> <swp0_sz> <swp1_sz> <swp2_sz> <swp3_sz>
<start_time_0>  <proc_path_0>  <priority_0>
<start_time_1>  <proc_path_1>  <priority_1>
...
```

Example (`input/os_1_mlq_paging`):

```
2 4 8
268435456 16777216 0 0 0
1 p1s   15
5 m0s   120
7 p0s   130
…
```

**Legacy format** (`MM_FIXED_MEMSZ` enabled in `include/os-cfg.h`):

```
<time_slice> <num_cpus> <num_processes>
<start_time_0>  <proc_path_0>  <priority_0>
...
```

Used by the `sched_*` configurations that focus on scheduling and do
not need the explicit memory-size line.

## Compile-time switches

`include/os-cfg.h`:

| Macro | Purpose |
|---|---|
| `MLQ_SCHED` | Enables the multi-level queue scheduler. |
| `MAX_PRIO` | Priority levels (140 to match the Linux reference). |
| `MM_PAGING` | Enables the paging memory manager (vs. legacy `mem.c`). |
| `MM_FIXED_MEMSZ` | Use hard-coded memory sizes; required for legacy `sched_*` configs. |
| `MM64` | Use the 64-bit address scheme (`mm64.c`). |
| `IODUMP` | Verbose `PID=… alloc/read/write` logging. |
| `PAGETBL_DUMP` | Print the page table after every alloc/free/read/write. |

`include/mm.h` exposes the unified `PAGING_PAGESZ = 256` constant used
throughout the paging path. `include/mm64.h` keeps the 4KB
`PAGING64_PAGESZ` for the architectural bit layout.

## Tests

The bundled regression set under `input/`:

| Config | Focus |
|---|---|
| `sched`, `sched_0`, `sched_1` | Scheduler only (`MM_FIXED_MEMSZ` mode). |
| `os_0_mlq_paging` | Small paging workload, 1 CPU. |
| `os_1_mlq_paging`, `os_1_singleCPU_mlq_paging` | Larger paging workload. |
| `os_1_mlq_paging_small_1K`, `_4K` | Very small RAM stress. |
| `os_2_mlq_paging`, `os_2_singleCPU_mlq_paging` | Adds kmem-using programs (`m2s`). |
| `os_sc`, `os_syscall`, `os_syscall_list` | Direct `syscall` instruction. |
| `os_kmem` | End-to-end kmem flow (alloc → copy_from_user → cache_create → cache_alloc). |

Sample reference outputs live in `output/`. Because the simulator is
multi-threaded, exact interleavings differ between runs — outputs are
samples, not golden values.

## Synchronisation model

- `queue_lock` (sched.c) — serialises `add_proc / get_proc / put_proc`.
- `mmvm_lock` (libmem.c) — serialises every user-side memory operation
  (`alloc / free / read / write` and kernel copies).
- `mem_lock` (mm-memphy.c) — serialises every byte and frame-list
  access on a `memphy_struct`.
- `kmem_lock` (libmem.c) — serialises the kernel-pool bump pointer and
  the per-pool slot counter.

Lock ordering used throughout: **`mmvm_lock` → `mem_lock`** and
**`kmem_lock` released before taking `mmvm_lock`** to avoid hold-and-wait
cycles.

## Memory hygiene

- `free_pcb` (libmem.c) is called from `cpu_routine` when a process
  finishes, and releases:
  - physical frames back to MEMRAM / MEMSWP (`free_pcb_memph`),
  - the page-table arrays (`pgd/p4d/pud/pmd/pt`),
  - the VMA list with its free-region list,
  - the FIFO page list,
  - the code segment, page table and PCB itself.
- `finish_memphy` (mm-memphy.c) releases free / used frame lists and
  the storage buffer.
- The OS `main()` calls `finish_memphy`, frees kernel page-table
  arrays, the load-args struct and the CPU descriptors, and calls
  `finish_scheduler` before exiting.

macOS `leaks` reports **0 bytes leaked** on `os_sc`, `os_kmem`,
`os_1_mlq_paging` and `os_2_singleCPU_mlq_paging`.

## Known limitations

- The page table is allocated as a flat `pt[]` array of
  `PAGING64_MAX_PGN` entries rather than a true 5-level structure with
  on-demand directories. Print path walks the 5 indices for display
  only.
- `enlist_vm_freerg_list` does not coalesce adjacent free regions;
  fragmentation grows under long-running alloc/free churn.
- The simulator uses a single VMA per process (`vmaid = 0`).
- The kernel-memory bump allocator never reclaims; per-process kernel
  allocations are released only at OS exit.
- FIFO is the only implemented page-replacement policy.

## References

- Course: HCMUT CSE — **CO2018 Operating Systems**, Spring 2026.
- Spec: *Caitoa* release of the Simple Operating System assignment
  (`CO2018_ossim_caitoa.pdf`).
- Skeleton authors: pdnguyen, Minh Thanh CHUNG, Hai Duc NGUYEN.
- License: Course-restricted study use only — see the licence grant
  in each source file.
