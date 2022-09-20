#include "ccv.h"
#include "ccv_internal.h"
#include "nnc/ccv_nnc.h"
#include "nnc/ccv_nnc_easy.h"
#include "nnc/ccv_nnc_internal.h"
#ifdef USE_OPENMP
#include <omp.h>
#endif
#ifdef USE_DISPATCH
#include <dispatch/dispatch.h>
#endif

static int _ccv_nnc_binary_crossentropy_forw(const ccv_nnc_cmd_t cmd, const ccv_nnc_hint_t hint, const int flags, ccv_nnc_tensor_t* const* const inputs, const int input_size, ccv_nnc_tensor_t* const* const outputs, const int output_size, ccv_nnc_stream_context_t* const stream_context)
{
	assert(input_size == 2);
	const ccv_nnc_tensor_view_t* a = (ccv_nnc_tensor_view_t*)inputs[0];
	assert(ccv_nnc_tensor_nd(a->info.dim) <= 2);
	const ccv_nnc_tensor_view_t* b = (ccv_nnc_tensor_view_t*)inputs[1];
	assert(output_size == 1);
	ccv_nnc_tensor_view_t* c = (ccv_nnc_tensor_view_t*)outputs[0];
	int dim[CCV_NNC_MAX_DIM_ALLOC];
	int astride[CCV_NNC_MAX_DIM_ALLOC];
	int bstride[CCV_NNC_MAX_DIM_ALLOC];
	int cstride[CCV_NNC_MAX_DIM_ALLOC];
	ccv_nnc_tensor_view_get_dim(a, dim);
	assert(ccv_nnc_tensor_view_check_dim(b, dim));
	ccv_nnc_tensor_view_get_stride(a, astride);
	ccv_nnc_tensor_view_get_stride(b, bstride);
	ccv_nnc_tensor_view_get_stride(c, cstride);
	assert(ccv_nnc_tensor_nd(a->info.dim) <= 2);
	const int batch_size = dim[CCV_NNC_MAX_DIM];
	assert(ccv_nnc_tensor_count(c->info) == batch_size);
	const int count = dim[CCV_NNC_MAX_DIM + 1];
	const int astep = astride[CCV_NNC_MAX_DIM];
	const int bstep = bstride[CCV_NNC_MAX_DIM];
	const int cstep = ccv_nnc_tensor_nd(c->info.dim) == 1 ? 1 : cstride[CCV_NNC_MAX_DIM];
	const float pos_weight = cmd.info.binary_crossentropy.pos_weight;
	if (pos_weight == 1)
	{
		parallel_for(i, batch_size) {
			int j;
			const float* const ap = a->data.f32 + i * astep;
			const float* const bp = b->data.f32 + i * bstep;
			float cp = 0;
			for (j = 0; j < count; j++)
				cp += (bp[j] - 1) * log(1 - ap[j]) - bp[j] * log(ap[j]);
			c->data.f32[i * cstep] = cp;
		} parallel_endfor
	} else {
		parallel_for(i, batch_size) {
			int j;
			const float* const ap = a->data.f32 + i * astep;
			const float* const bp = b->data.f32 + i * bstep;
			float cp1 = 0, cp2 = 0;
			for (j = 0; j < count; j++)
				cp1 += (bp[j] - 1) * log(1 - ap[j]), cp2 += bp[j] * log(ap[j]);
			c->data.f32[i * cstep] = cp1 - cp2 * pos_weight;
		} parallel_endfor
	}
	return CCV_NNC_EXEC_SUCCESS;
}

static int _ccv_nnc_binary_crossentropy_back(const ccv_nnc_cmd_t cmd, const ccv_nnc_hint_t hint, const int flags, ccv_nnc_tensor_t* const* const inputs, const int input_size, ccv_nnc_tensor_t* const* const outputs, const int output_size, ccv_nnc_stream_context_t* const stream_context)
{
	assert(input_size >= 3);
	assert(output_size >= 1);
	const ccv_nnc_tensor_view_t* const g = (ccv_nnc_tensor_view_t*)inputs[0];
	assert(!g || !CCV_IS_TENSOR_VIEW(g));
	const ccv_nnc_tensor_view_t* const a = (ccv_nnc_tensor_view_t*)inputs[1];
	const ccv_nnc_tensor_view_t* const b = (ccv_nnc_tensor_view_t*)inputs[2];
	ccv_nnc_tensor_view_t* const h = (ccv_nnc_tensor_view_t*)outputs[0];
	int dim[CCV_NNC_MAX_DIM_ALLOC];
	int astride[CCV_NNC_MAX_DIM_ALLOC];
	int bstride[CCV_NNC_MAX_DIM_ALLOC];
	int hstride[CCV_NNC_MAX_DIM_ALLOC];
	ccv_nnc_tensor_view_get_dim(a, dim);
	assert(ccv_nnc_tensor_view_check_dim(b, dim));
	assert(ccv_nnc_tensor_view_check_dim(h, dim));
	ccv_nnc_tensor_view_get_stride(a, astride);
	ccv_nnc_tensor_view_get_stride(b, bstride);
	ccv_nnc_tensor_view_get_stride(h, hstride);
	assert(ccv_nnc_tensor_nd(a->info.dim) <= 2);
	const int batch_size = dim[CCV_NNC_MAX_DIM];
	const int count = dim[CCV_NNC_MAX_DIM + 1];
	const int astep = astride[CCV_NNC_MAX_DIM];
	const int bstep = bstride[CCV_NNC_MAX_DIM];
	const int hstep = hstride[CCV_NNC_MAX_DIM];
	const float pos_weight = cmd.info.binary_crossentropy.pos_weight;
	if (pos_weight == 1)
	{
		if (g)
		{
			int gstride[CCV_NNC_MAX_DIM_ALLOC];
			ccv_nnc_tensor_view_get_stride(g, gstride);
			assert(ccv_nnc_tensor_count(g->info) == batch_size);
			const int gstep = ccv_nnc_tensor_nd(g->info.dim) == 1 ? 1 : gstride[CCV_NNC_MAX_DIM];
			parallel_for(i, batch_size) {
				int j;
				const float gp = g->data.f32[i * gstep];
				const float* const ap = a->data.f32 + i * astep;
				const float* const bp = b->data.f32 + i * bstep;
				float* const hp = h->data.f32 + i * hstep;
				for (j = 0; j < count; j++)
					hp[j] = gp * (ap[j] - bp[j]) / ccv_max((1 - ap[j]) * ap[j], 1e-12);
			} parallel_endfor
		} else {
			parallel_for(i, batch_size) {
				int j;
				const float* const ap = a->data.f32 + i * astep;
				const float* const bp = b->data.f32 + i * bstep;
				float* const hp = h->data.f32 + i * hstep;
				for (j = 0; j < count; j++)
					hp[j] = (ap[j] - bp[j]) / ccv_max((1 - ap[j]) * ap[j], 1e-12);
			} parallel_endfor
		}
	} else {
		const float pos_weight_1 = pos_weight - 1;
		if (g)
		{
			int gstride[CCV_NNC_MAX_DIM_ALLOC];
			ccv_nnc_tensor_view_get_stride(g, gstride);
			assert(ccv_nnc_tensor_count(g->info) == batch_size);
			const int gstep = ccv_nnc_tensor_nd(g->info.dim) == 1 ? 1 : gstride[CCV_NNC_MAX_DIM];
			parallel_for(i, batch_size) {
				int j;
				const float gp = g->data.f32[i * gstep];
				const float* const ap = a->data.f32 + i * astep;
				const float* const bp = b->data.f32 + i * bstep;
				float* const hp = h->data.f32 + i * hstep;
				for (j = 0; j < count; j++)
					hp[j] = gp * (ap[j] * bp[j] * pos_weight_1 + ap[j] - pos_weight * bp[j]) / ccv_max((1 - ap[j]) * ap[j], 1e-12);
			} parallel_endfor
		} else {
			parallel_for(i, batch_size) {
				int j;
				const float* const ap = a->data.f32 + i * astep;
				const float* const bp = b->data.f32 + i * bstep;
				float* const hp = h->data.f32 + i * hstep;
				for (j = 0; j < count; j++)
					hp[j] = (ap[j] * bp[j] * pos_weight_1 + ap[j] - pos_weight * bp[j]) / ccv_max((1 - ap[j]) * ap[j], 1e-12);
			} parallel_endfor
		}
	}
	return CCV_NNC_EXEC_SUCCESS;
}

REGISTER_COMMAND_BACKEND(CCV_NNC_BINARY_CROSSENTROPY_FORWARD, CCV_NNC_BACKEND_CPU_REF)(ccv_nnc_cmd_backend_registry_t* const registry)
{
	registry->tensor_formats = CCV_TENSOR_FORMAT_NHWC | CCV_TENSOR_FORMAT_NCHW;
	registry->tensor_datatypes = CCV_32F;
	registry->tensor_memory = CCV_TENSOR_CPU_MEMORY;
	registry->algorithms = 1;
	registry->exec = _ccv_nnc_binary_crossentropy_forw;
}

REGISTER_COMMAND_BACKEND(CCV_NNC_BINARY_CROSSENTROPY_BACKWARD, CCV_NNC_BACKEND_CPU_REF)(ccv_nnc_cmd_backend_registry_t* const registry)
{
	registry->tensor_formats = CCV_TENSOR_FORMAT_NHWC | CCV_TENSOR_FORMAT_NCHW;
	registry->tensor_datatypes = CCV_32F;
	registry->tensor_memory = CCV_TENSOR_CPU_MEMORY;
	registry->algorithms = 1;
	registry->exec = _ccv_nnc_binary_crossentropy_back;
}
