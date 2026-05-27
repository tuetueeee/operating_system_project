/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "queue.h"
#include "sched.h"
#include <pthread.h>

#include <stdlib.h>
#include <stdio.h>
static struct queue_t ready_queue;
static struct queue_t run_queue;
static pthread_mutex_t queue_lock;

static struct queue_t running_list;
#ifdef MLQ_SCHED
static struct queue_t mlq_ready_queue[MAX_PRIO];
static int slot[MAX_PRIO];
static int current_mlq_prio;
#endif

static void remove_from_running_list(struct pcb_t * proc) {
	if (proc == NULL)
		return;

	purgequeue(&running_list, proc);
}

#ifdef MLQ_SCHED
static void reset_mlq_slot(void) {
	int prio;

	for (prio = 0; prio < MAX_PRIO; prio++)
		slot[prio] = MAX_PRIO - prio;
}

static int normalize_prio(unsigned int prio) {
	if (prio >= MAX_PRIO) {
		fprintf(stderr, "sched: priority %u out of range, clamped to %d\n",
			prio, MAX_PRIO - 1);
		return MAX_PRIO - 1;
	}

	return (int)prio;
}
#endif

int queue_empty(void) {
#ifdef MLQ_SCHED
	unsigned long prio;
	for (prio = 0; prio < MAX_PRIO; prio++)
		if (!empty(&mlq_ready_queue[prio]))
			return 0;
#endif
	return (empty(&ready_queue) && empty(&run_queue));
}

void init_scheduler(void) {
#ifdef MLQ_SCHED
    int i ;

	for (i = 0; i < MAX_PRIO; i ++) {
		mlq_ready_queue[i].size = 0;
		slot[i] = MAX_PRIO - i; 
	}
	current_mlq_prio = 0;
#endif
	ready_queue.size = 0;
	run_queue.size = 0;
	running_list.size = 0;
	pthread_mutex_init(&queue_lock, NULL);
}

#ifdef MLQ_SCHED
/* 
 *  Stateful design for routine calling
 *  based on the priority and our MLQ policy
 *  We implement stateful here using transition technique
 *  State representation   prio = 0 .. MAX_PRIO, curr_slot = 0..(MAX_PRIO - prio)
 */
struct pcb_t * get_mlq_proc(void) {
	struct pcb_t * proc = NULL;
	int scanned;

	if (queue_empty()) {
		return NULL;
	}

	for (scanned = 0; scanned < MAX_PRIO; scanned++) {
		int prio = (current_mlq_prio + scanned) % MAX_PRIO;

		if (slot[prio] <= 0 || empty(&mlq_ready_queue[prio]))
			continue;

		proc = dequeue(&mlq_ready_queue[prio]);
		slot[prio]--;
		current_mlq_prio = prio;

		if (slot[prio] == 0 || empty(&mlq_ready_queue[prio]))
			current_mlq_prio = (prio + 1) % MAX_PRIO;

		break;
	}

	if (proc == NULL) {
		reset_mlq_slot();
		for (scanned = 0; scanned < MAX_PRIO; scanned++) {
			int prio = (current_mlq_prio + scanned) % MAX_PRIO;

			if (empty(&mlq_ready_queue[prio]))
				continue;

			proc = dequeue(&mlq_ready_queue[prio]);
			slot[prio]--;
			current_mlq_prio = prio;

			if (slot[prio] == 0 || empty(&mlq_ready_queue[prio]))
				current_mlq_prio = (prio + 1) % MAX_PRIO;

			break;
		}
	}

	return proc;
}

void put_mlq_proc(struct pcb_t * proc) {
	int prio;

	if (proc == NULL)
		return;

	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->mlq_ready_queue = mlq_ready_queue;
	proc->krnl->running_list = &running_list;
	prio = normalize_prio(proc->prio);
	proc->prio = prio;

	enqueue(&mlq_ready_queue[prio], proc);
}

void add_mlq_proc(struct pcb_t * proc) {
	int prio;

	if (proc == NULL)
		return;

	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->mlq_ready_queue = mlq_ready_queue;
	proc->krnl->running_list = &running_list;
	prio = normalize_prio(proc->prio);
	proc->prio = prio;

	enqueue(&mlq_ready_queue[prio], proc);
	enqueue(&running_list, proc);
}

struct pcb_t * get_proc(void) {
	pthread_mutex_lock(&queue_lock);
	struct pcb_t * proc = get_mlq_proc();
	pthread_mutex_unlock(&queue_lock);
	return proc;
}

void put_proc(struct pcb_t * proc) {
	if (proc == NULL)
		return;
	pthread_mutex_lock(&queue_lock);
	put_mlq_proc(proc);
	pthread_mutex_unlock(&queue_lock);
}

void add_proc(struct pcb_t * proc) {
	if (proc == NULL)
		return;
	pthread_mutex_lock(&queue_lock);
	add_mlq_proc(proc);
	pthread_mutex_unlock(&queue_lock);
}
#else
struct pcb_t * get_proc(void) {
	struct pcb_t * proc = NULL;

	pthread_mutex_lock(&queue_lock);
	if (!empty(&ready_queue)) {
		proc = dequeue(&ready_queue);
	}
	pthread_mutex_unlock(&queue_lock);

	return proc;
}

void put_proc(struct pcb_t * proc) {
	if (proc == NULL)
		return;

	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;

	pthread_mutex_lock(&queue_lock);
	enqueue(&run_queue, proc);
	pthread_mutex_unlock(&queue_lock);
}

void add_proc(struct pcb_t * proc) {
	if (proc == NULL)
		return;

	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;

	pthread_mutex_lock(&queue_lock);
	enqueue(&ready_queue, proc);
	enqueue(&running_list, proc);
	pthread_mutex_unlock(&queue_lock);	
}
#endif

void remove_proc(struct pcb_t * proc) {
	pthread_mutex_lock(&queue_lock);
	remove_from_running_list(proc);
	pthread_mutex_unlock(&queue_lock);
}

struct pcb_t * sched_find_proc_by_pid(struct krnl_t *krnl, uint32_t pid) {
	if (krnl == NULL)
		return NULL;
	/* Use the per-krnl running_list pointer (set by add_proc) rather
	 * than the file-scope `running_list` directly, so this helper would
	 * still work if multiple kernel instances ever shared this scheduler
	 * file. */
	struct queue_t *list = krnl->running_list;
	if (list == NULL)
		return NULL;

	struct pcb_t *match = NULL;
	pthread_mutex_lock(&queue_lock);
	for (int i = 0; i < list->size; i++) {
		if (list->proc[i] != NULL && list->proc[i]->pid == pid) {
			match = list->proc[i];
			break;
		}
	}
	pthread_mutex_unlock(&queue_lock);
	return match;
}

void finish_scheduler(void) {
	pthread_mutex_destroy(&queue_lock);
}


