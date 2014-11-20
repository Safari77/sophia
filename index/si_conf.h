#ifndef SI_CONF_H_
#define SI_CONF_H_

/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

typedef struct siconf siconf;

struct siconf {
	char     *path;
	int       read_only;
	int       create;
	int       sync;
	uint64_t  memory_limit;
	uint32_t  node_size;
	uint32_t  node_page_size;
	uint32_t  node_branch_wm;
	uint32_t  node_merge_wm;
};

#endif
