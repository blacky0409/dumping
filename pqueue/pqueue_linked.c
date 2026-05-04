#include "pqueue_linked.h"

void pq_L_init(pqueue_L *pq){
	printk(KERN_INFO "hey \n");
	INIT_LIST_HEAD(&pq->head);
	printk(KERN_INFO "what the \n");
	pq->count=0;
	printk(KERN_INFO "fuck \n");
}

int pq_L_is_empty(pqueue_L *pq){
	return list_empty(&pq->head);
}

void pq_L_insert(pqueue_L *pq, linked_Node *node){
	linked_Node * pos;
	
	list_for_each_entry(pos, &pq->head, list){
		if(node->size > pos->size){
			list_add_tail(&node->list, &pos->list);
			pq->count++;
			return;
		}
	}
	
	list_add_tail(&node->list, &pq->head);
	return;
}

linked_Node * pq_L_pop(pqueue_L *pq){
	linked_Node * node = NULL;
	
	if(pq_L_is_empty(pq)){
		printk(KERN_INFO "Priority Queue is empty\n");
		return NULL;
	}

	node = list_first_entry(&pq->head, linked_Node, list);
	list_del(&node->list);

	return node;
}

int pq_L_size(pqueue_L *pq){
	return pq->count;
}

