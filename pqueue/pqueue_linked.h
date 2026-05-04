#ifndef PQUEUE_L
#define PQUEUE_L

#include "../nvmev.h"


typedef struct contiguous_lpn{
	struct list_head list;

	/* data */
	uint64_t start_lpn;
	uint64_t end_lpn;
	int part;
	/*      */ 		

	uint64_t size; /*priority*/

} linked_Node;

typedef struct {
	struct list_head head;
	int count;
} pqueue_L;


void pq_L_init(pqueue_L *pq);

int pq_L_is_empty(pqueue_L *pq);

void pq_L_insert(pqueue_L *pq, linked_Node *node);

linked_Node * pq_L_pop(pqueue_L *pq);

int pq_L_size(pqueue_L *pq);
#endif /* PQUEUE_L */
