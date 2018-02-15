/* This file implements a singly link list */
#include <stdlib.h>
#include <string.h>
#include <assert.h>
 
#include "list.h"
 
typedef struct list_node {
	void *data;
	struct list_node *next;
} list_node_t;

void list_new(list_t *list, list_free_t free_fn)
{
	list->len = 0;
	list->head = list->tail = NULL;
	list->free_fn = free_fn;
}
 
void list_destroy(list_t *list)
{
	list_node_t *current;
	
	while (list->head != NULL) {
		
		current = list->head;
		list->head = current->next;

		if (list->free_fn)
			list->free_fn(current->data);

		free(current);
	}
}
 
bool list_prepend(list_t *list, void *element)
{

	list_node_t *node = malloc(sizeof(list_node_t));
	if (node == NULL)
		return false;

	node->data = element;

	node->next = list->head;
	list->head = node;

	/* first node? */
	if (!list->tail) {
		list->tail = list->head;
	}

	list->len++;
	return true;
}
 
bool list_append(list_t *list, void *element)
{
	list_node_t *node = malloc(sizeof(list_node_t));
	if (node == NULL)
		return false;

	node->data = element;
	node->next = NULL;

	if (list->len == 0) {
		list->head = list->tail = node;
	} else {
    		list->tail->next = node;
		list->tail = node;
	}

	list->len++;
	
	return true;
}
 
void list_for_each(list_t *list, list_iterator_t iterator)
{
	assert(iterator != NULL);
 
	list_node_t *node = list->head;
	
	bool result = true;
	while (node != NULL && result) {
		result = iterator(node->data);
		node = node->next;
	}
}

/* This function doesn't call free_fn */
void *list_pop_head(list_t *list)
{
	void *data;
	if (list->len == 0)
		return NULL;

	data = list->head;
	list->head = list->head->next;

	return data;
}

int list_size(list_t *list)
{
  return list->len;
}
