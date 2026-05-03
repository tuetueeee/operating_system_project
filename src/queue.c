#include <stdio.h>
#include <stdlib.h>
#include "queue.h"

int empty(struct queue_t *q)
{
        if (q == NULL)
                return 1;
        return (q->size == 0);
}

void enqueue(struct queue_t *q, struct pcb_t *proc)
{
        if (q == NULL || proc == NULL)
                return;

        if (q->size >= MAX_QUEUE_SIZE) {
                fprintf(stderr, "enqueue: queue overflow\n");
                return;
        }

        q->proc[q->size++] = proc;
}

struct pcb_t *dequeue(struct queue_t *q)
{
        struct pcb_t *proc;

        if (empty(q))
                return NULL;

        proc = q->proc[0];
        for (int i = 1; i < q->size; i++)
                q->proc[i - 1] = q->proc[i];

        q->size--;
        return proc;
}

struct pcb_t *purgequeue(struct queue_t *q, struct pcb_t *proc)
{
        if (empty(q) || proc == NULL)
                return NULL;

        int idx = -1;
        for (int i = 0; i < q->size; i++) {
                if (q->proc[i] == proc) {
                        idx = i;
                        break;
                }
        }

        if (idx < 0)
                return NULL;

        for (int i = idx + 1; i < q->size; i++)
                q->proc[i - 1] = q->proc[i];

        q->size--;
        return proc;
}