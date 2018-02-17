#ifndef __LIST_H__
#define __LIST_H__

#include <stdbool.h>

/* a common function used to free malloc'd objects */
typedef void (*list_free_t)(void *);

/* Iterator for list */
typedef bool (*list_iterator_t)(void *);
 
typedef struct list_node list_node_t;
 
typedef struct {
	int len;
	list_node_t *head;
	list_node_t *tail;
	list_free_t free_fn;
} list_t;
 
void list_new(list_t *list, list_free_t free_fn);
void list_destroy(list_t *list);
 
bool list_prepend(list_t *list, void *element);
bool list_append(list_t *list, void *element);
int list_size(list_t *list);
 
void list_for_each(list_t *list, list_iterator_t iterator);
void *list_pop_head(list_t *list);

/* Remove first entry matching data */
bool list_remove(list_t *list, void *data);

#endif /* __LIST_H__ */
