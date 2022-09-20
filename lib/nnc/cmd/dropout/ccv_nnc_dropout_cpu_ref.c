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
#include "3rdparty/dsfmt/dSFMT.h"

// Shared methods.
#include "../_ccv_nnc_cpu_ref.h"

static int _ccv_nnc_dropout_forw(const ccv_nnc_cmd_t cmd, const ccv_nnc_hint_t hint, const int flags, ccv_nnc_tensor_t* const* const inputs, const int input_size, ccv_nnc_tensor_t* const* const outputs, const int output_size, ccv_nnc_stream_context_t* const stream_context)
{
	const float p = cmd.info.dropout.p;
	const float inv_p = 1. / (1. - p);
	assert(output_size >= 2);
	// Assuming this is float 32.
	int dim[CCV_NNC_MAX_DIM_ALLOC];
	int astride[CCV_NNC_MAX_DIM_ALLOC];
	int bstride[CCV_NNC_MAX_DIM_ALLOC];
	ccv_nnc_tensor_view_t* a = (ccv_nnc_tensor_view_t*)inputs[0];
	ccv_nnc_tensor_view_t* b = (ccv_nnc_tensor_view_t*)outputs[0];
	assert(ccv_nnc_tensor_nd(a->info.dim) <= CCV_NNC_MAX_DIM + 2);
	assert(ccv_nnc_tensor_nd(b->info.dim) <= CCV_NNC_MAX_DIM + 2);
	ccv_nnc_tensor_view_get_dim(a, dim);
	assert(ccv_nnc_tensor_view_check_dim(b, dim));
	const int tensor_count = ccv_nnc_tensor_count(inputs[0]->info);
	uint8_t* const maskdata = outputs[1]->data.u8;
	dsfmt_t dsfmt;
	dsfmt_init_gen_rand(&dsfmt, ccv_nnc_stream_context_genrand_uint32(stream_context));
	int x;
	if (cmd.info.dropout.entirety)
	{
		const int32_t drop = ((int32_t*)maskdata)[0] = (dsfmt_genrand_open_close(&dsfmt) <= p);
		if (!CCV_IS_TENSOR_VIEW(a) && !CCV_IS_TENSOR_VIEW(b))
		{
			// Super optimal case, just do one for-loop for sum.
			for (x = 0; x < tensor_count; x++)
				b->data.f32[x] = drop ? 0 : a->data.f32[x] * inv_p;
			return CCV_NNC_EXEC_SUCCESS;
		}
		assert(CCV_NNC_MAX_DIM == 2); // Need to change this logic for CCV_NNC_MAX_DIM == other number.
		ccv_nnc_tensor_view_get_stride(a, astride);
		ccv_nnc_tensor_view_get_stride(b, bstride);
		int i[CCV_NNC_MAX_DIM + 2];
		float* const ap = a->data.f32;
		float* const bp = b->data.f32;
		const int count = dim[2] * dim[3];
		if (astride[2] == dim[3] && bstride[2] == dim[3])
		{
			// Special casing if the ainc[3] is the same as dim[3]
			for (i[0] = 0; i[0] < dim[0]; i[0]++)
			{
				float* ap0 = ap + i[0] * astride[0];
				float* bp0 = bp + i[0] * bstride[0];
				for (i[1] = 0; i[1] < dim[1]; i[1]++)
				{
					for (x = 0; x < count; x++)
						bp0[x] = drop ? 0 : ap0[x] * inv_p;
					ap0 += astride[1];
					bp0 += bstride[1];
				}
			}
			return CCV_NNC_EXEC_SUCCESS;
		}
		// Non-optimal case, need to do skip copy.
		for (i[0] = 0; i[0] < dim[0]; i[0]++)
		{
			float* const ap0 = ap + i[0] * astride[0];
			float* const bp0 = bp + i[0] * bstride[0];
			for (i[1] = 0; i[1] < dim[1]; i[1]++)
			{
				float* ap1 = ap0 + i[1] * astride[1];
				float* bp1 = bp0 + i[1] * bstride[1];
				for (i[2] = 0; i[2] < dim[2]; i[2]++)
				{
					for (x = 0; x < dim[3]; x++)
						bp1[x] = drop ? 0 : ap1[x] * inv_p;
					ap1 += astride[2];
					bp1 += bstride[2];
				}
			}
		}
	} else {
		uint8_t* maskp = maskdata + (tensor_count - 1);
		for (; maskp >= maskdata; --maskp)
			*maskp = (dsfmt_genrand_open_close(&dsfmt) <= p);
		if (!CCV_IS_TENSOR_VIEW(a) && !CCV_IS_TENSOR_VIEW(b))
		{
			// Super optimal case, just do one for-loop for sum.
			for (x = 0; x < tensor_count; x++)
				b->data.f32[x] = maskdata[x] ? 0 : a->data.f32[x] * inv_p;
			return CCV_NNC_EXEC_SUCCESS;
		}
		assert(CCV_NNC_MAX_DIM == 2); // Need to change this logic for CCV_NNC_MAX_DIM == other number.
		ccv_nnc_tensor_view_get_stride(a, astride);
		ccv_nnc_tensor_view_get_stride(b, bstride);
		int i[CCV_NNC_MAX_DIM + 2];
		float* const ap = a->data.f32;
		float* const bp = b->data.f32;
		const int count = dim[2] * dim[3];
		maskp = maskdata;
		if (astride[2] == dim[3] && bstride[2] == dim[3])
		{
			// Special casing if the ainc[3] is the same as dim[3]
			for (i[0] = 0; i[0] < dim[0]; i[0]++)
			{
				float* ap0 = ap + i[0] * astride[0];
				float* bp0 = bp + i[0] * bstride[0];
				for (i[1] = 0; i[1] < dim[1]; i[1]++)
				{
					for (x = 0; x < count; x++)
						bp0[x] = maskp[x] ? 0 : ap0[x] * inv_p;
					ap0 += astride[1];
					bp0 += bstride[1];
					maskp += count;
				}
			}
			return CCV_NNC_EXEC_SUCCESS;
		}
		// Non-optimal case, need to do skip copy.
		for (i[0] = 0; i[0] < dim[0]; i[0]++)
		{
			float* const ap0 = ap + i[0] * astride[0];
			float* const bp0 = bp + i[0] * bstride[0];
			for (i[1] = 0; i[1] < dim[1]; i[1]++)
			{
				float* ap1 = ap0 + i[1] * astride[1];
				float* bp1 = bp0 + i[1] * bstride[1];
				for (i[2] = 0; i[2] < dim[2]; i[2]++)
				{
					for (x = 0; x < dim[3]; x++)
						bp1[x] = maskp[x] ? 0 : ap1[x] * inv_p;
					maskp += dim[3];
					ap1 += astride[2];
					bp1 += bstride[2];
				}
			}
		}
	}
	return CCV_NNC_EXEC_SUCCESS;
}

static int _ccv_nnc_dropout_back(const ccv_nnc_cmd_t cmd, const ccv_nnc_hint_t hint, const int flags, ccv_nnc_tensor_t* const* const inputs, const int input_size, ccv_nnc_tensor_t* const* const outputs, const int output_size, ccv_nnc_stream_context_t* const stream_context)
{
	assert(input_size == 5);
	const float p = cmd.info.dropout.p;
	const float inv_p = 1. / (1. - p);
	uint8_t* const maskdata = inputs[4]->data.u8;
	// Assuming this is float 32.
	int dim[CCV_NNC_MAX_DIM_ALLOC];
	int gstride[CCV_NNC_MAX_DIM_ALLOC];
	int hstride[CCV_NNC_MAX_DIM_ALLOC];
	ccv_nnc_tensor_view_t* g = (ccv_nnc_tensor_view_t*)inputs[0];
	ccv_nnc_tensor_view_t* h = (ccv_nnc_tensor_view_t*)outputs[0];
	assert(ccv_nnc_tensor_nd(g->info.dim) <= CCV_NNC_MAX_DIM + 2);
	assert(ccv_nnc_tensor_nd(h->info.dim) <= CCV_NNC_MAX_DIM + 2);
	ccv_nnc_tensor_view_get_dim(g, dim);
	assert(ccv_nnc_tensor_view_check_dim(h, dim));
	int x;
	if (cmd.info.dropout.entirety)
	{
		const int32_t drop = ((int32_t*)maskdata)[0];
		if (!CCV_IS_TENSOR_VIEW(g) && !CCV_IS_TENSOR_VIEW(h))
		{
			// Super optimal case, just do one for-loop for sum.
			const int tensor_count = ccv_nnc_tensor_count(inputs[0]->info);
			for (x = 0; x < tensor_count; x++)
				h->data.f32[x] = drop ? 0 : g->data.f32[x] * inv_p;
			return CCV_NNC_EXEC_SUCCESS;
		}
		assert(CCV_NNC_MAX_DIM == 2); // Need to change this logic for CCV_NNC_MAX_DIM == other number.
		ccv_nnc_tensor_view_get_stride(g, gstride);
		ccv_nnc_tensor_view_get_stride(h, hstride);
		int i[CCV_NNC_MAX_DIM + 2];
		float* const gp = g->data.f32;
		float* const hp = h->data.f32;
		const int count = dim[2] * dim[3];
		if (gstride[2] == dim[3] && hstride[2] == dim[3])
		{
			// Special casing if the ginc[3] is the same as dim[3]
			for (i[0] = 0; i[0] < dim[0]; i[0]++)
			{
				float* gp0 = gp + i[0] * gstride[0];
				float* hp0 = hp + i[0] * hstride[0];
				for (i[1] = 0; i[1] < dim[1]; i[1]++)
				{
					for (x = 0; x < count; x++)
						hp0[x] = drop ? 0 : gp0[x] * inv_p;
					gp0 += gstride[1];
					hp0 += hstride[1];
				}
			}
			return CCV_NNC_EXEC_SUCCESS;
		}
		// Non-optimal case, need to do skip copy.
		for (i[0] = 0; i[0] < dim[0]; i[0]++)
		{
			float* const gp0 = gp + i[0] * gstride[0];
			float* const hp0 = hp + i[0] * hstride[0];
			for (i[1] = 0; i[1] < dim[1]; i[1]++)
			{
				float* gp1 = gp0 + i[1] * gstride[1];
				float* hp1 = hp0 + i[1] * hstride[1];
				for (i[2] = 0; i[2] < dim[2]; i[2]++)
				{
					for (x = 0; x < dim[3]; x++)
						hp1[x] = drop ? 0 : gp1[x] * inv_p;
					gp1 += gstride[2];
					hp1 += hstride[2];
				}
			}
		}
	} else {
		if (!CCV_IS_TENSOR_VIEW(g) && !CCV_IS_TENSOR_VIEW(h))
		{
			// Super optimal case, just do one for-loop for sum.
			const int tensor_count = ccv_nnc_tensor_count(inputs[0]->info);
			for (x = 0; x < tensor_count; x++)
				h->data.f32[x] = maskdata[x] ? 0 : g->data.f32[x] * inv_p;
			return CCV_NNC_EXEC_SUCCESS;
		}
		assert(CCV_NNC_MAX_DIM == 2); // Need to change this logic for CCV_NNC_MAX_DIM == other number.
		ccv_nnc_tensor_view_get_stride(g, gstride);
		ccv_nnc_tensor_view_get_stride(h, hstride);
		int i[CCV_NNC_MAX_DIM + 2];
		float* const gp = g->data.f32;
		float* const hp = h->data.f32;
		const int count = dim[2] * dim[3];
		uint8_t* maskp = maskdata;
		if (gstride[2] == dim[3] && hstride[2] == dim[3])
		{
			// Special casing if the ginc[3] is the same as dim[3]
			for (i[0] = 0; i[0] < dim[0]; i[0]++)
			{
				float* gp0 = gp + i[0] * gstride[0];
				float* hp0 = hp + i[0] * hstride[0];
				for (i[1] = 0; i[1] < dim[1]; i[1]++)
				{
					for (x = 0; x < count; x++)
						hp0[x] = maskp[x] ? 0 : gp0[x] * inv_p;
					gp0 += gstride[1];
					hp0 += hstride[1];
					maskp += count;
				}
			}
			return CCV_NNC_EXEC_SUCCESS;
		}
		// Non-optimal case, need to do skip copy.
		for (i[0] = 0; i[0] < dim[0]; i[0]++)
		{
			float* const gp0 = gp + i[0] * gstride[0];
			float* const hp0 = hp + i[0] * hstride[0];
			for (i[1] = 0; i[1] < dim[1]; i[1]++)
			{
				float* gp1 = gp0 + i[1] * gstride[1];
				float* hp1 = hp0 + i[1] * hstride[1];
				for (i[2] = 0; i[2] < dim[2]; i[2]++)
				{
					for (x = 0; x < dim[3]; x++)
						hp1[x] = maskp[x] ? 0 : gp1[x] * inv_p;
					maskp += dim[3];
					gp1 += gstride[2];
					hp1 += hstride[2];
				}
			}
		}
	}
	return CCV_NNC_EXEC_SUCCESS;
}

REGISTER_COMMAND_BACKEND(CCV_NNC_DROPOUT_FORWARD, CCV_NNC_BACKEND_CPU_REF)(ccv_nnc_cmd_backend_registry_t* const registry)
{
	registry->tensor_formats = CCV_TENSOR_FORMAT_NHWC | CCV_TENSOR_FORMAT_NCHW | CCV_TENSOR_FORMAT_CHWN;
	registry->tensor_datatypes = CCV_32F;
	registry->tensor_memory = CCV_TENSOR_CPU_MEMORY;
	registry->algorithms = 1;
	registry->exec = _ccv_nnc_dropout_forw;
}

REGISTER_COMMAND_BACKEND(CCV_NNC_DROPOUT_BACKWARD, CCV_NNC_BACKEND_CPU_REF)(ccv_nnc_cmd_backend_registry_t* const registry)
{
	registry->tensor_formats = CCV_TENSOR_FORMAT_NHWC | CCV_TENSOR_FORMAT_NCHW | CCV_TENSOR_FORMAT_CHWN;
	registry->tensor_datatypes = CCV_32F;
	registry->tensor_memory = CCV_TENSOR_CPU_MEMORY;
	registry->algorithms = 1;
	registry->exec = _ccv_nnc_dropout_back;
}
