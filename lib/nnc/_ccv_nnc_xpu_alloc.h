/**********************************************************
 * C-based/Cached/Core Computer Vision Library
 * Liu Liu, 2010-02-01
 **********************************************************/

/**********************************************************
 * CCV - Neural Network Collection
 **********************************************************/

#ifndef GUARD_ccv_nnc_xpu_alloc_internal_h
#define GUARD_ccv_nnc_xpu_alloc_internal_h

#include "ccv_nnc.h"
#include "ccv_nnc_internal.h"
#include "_ccv_nnc_stream.h"
#include "3rdparty/khash/khash.h"
#include "3rdparty/jemalloc/rb.h"

typedef struct dy_alloc_metadata_s dy_alloc_metadata_t;
struct dy_alloc_metadata_s {
	int device;
	size_t size;
	intptr_t str;
	rb_node(dy_alloc_metadata_t) link;
	dy_alloc_metadata_t* next; // So I can chain them together.
	void* ptr;
};
typedef rb_tree(dy_alloc_metadata_t) dy_alloc_tree_t;
KHASH_MAP_INIT_INT(dy_dev, dy_alloc_tree_t);
typedef struct {
	int hook_id;
	khash_t(dy_dev)* dev;
} dy_str_t;
KHASH_MAP_INIT_INT64(dy_str, dy_str_t);
KHASH_MAP_INIT_INT64(dy_alloc, dy_alloc_metadata_t*);

typedef struct {
	int mp_hdr; // Memory pressure handler.
	khash_t(dy_str)* freed; // The freed memory allocations.
	khash_t(dy_alloc)* allocd; // The allocated memory.
} ccv_nnc_xpu_alloc_t;

void* ccv_nnc_xpu_alloc(ccv_nnc_xpu_alloc_t* const xpu_alloc, const int device, ccv_nnc_stream_context_t* const stream, const size_t size);
void ccv_nnc_xpu_free(ccv_nnc_xpu_alloc_t* const xpu_alloc, void* const ptr);
void ccv_nnc_xpu_alloc_destroy(ccv_nnc_xpu_alloc_t* const xpu_alloc);
void ccv_nnc_xpu_gc(const int device, ccv_nnc_xpu_alloc_t* const xpu_alloc);

#endif
