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

// Shared methods.
#include "../_ccv_nnc_cpu_ref.h"

void _ccv_nnc_reduce_sum_forw_cpu_ref(ccv_nnc_tensor_view_t* const a, ccv_nnc_tensor_view_t* const b)
{
	assert(ccv_nnc_tensor_nd(a->info.dim) <= CCV_NNC_MAX_DIM + 2);
	assert(ccv_nnc_tensor_nd(b->info.dim) <= CCV_NNC_MAX_DIM + 2);
	// Assuming this is float 32.
	int adim[CCV_NNC_MAX_DIM_ALLOC];
	int bdim[CCV_NNC_MAX_DIM_ALLOC];
	ccv_nnc_tensor_view_get_dim(a, adim);
	ccv_nnc_tensor_view_get_dim(b, bdim);
	assert(ccv_nnc_tensor_view_check_broadcast_dim(b, adim));
	int astride[CCV_NNC_MAX_DIM_ALLOC];
	int bstride[CCV_NNC_MAX_DIM_ALLOC];
	assert(CCV_NNC_MAX_DIM == 2); // Need to change this logic for CCV_NNC_MAX_DIM == other number.
	ccv_nnc_tensor_view_get_stride(a, astride);
	ccv_nnc_tensor_view_get_stride(b, bstride);
	int i[CCV_NNC_MAX_DIM + 2];
	int x;
	ccv_nnc_tensor_zero(b);
	float* const ap = a->data.f32;
	float* const bp = b->data.f32;
	// Non-optimal case, need to do skip if needed.
	for (i[0] = 0; i[0] < adim[0]; i[0]++)
	{
		float* const ap0 = ap + i[0] * astride[0];
		float* const bp0 = bdim[0] == 1 ? bp : bp + i[0] * bstride[0];
		for (i[1] = 0; i[1] < adim[1]; i[1]++)
		{
			float* ap1 = ap0 + i[1] * astride[1];
			float* const bp1 = bdim[1] == 1 ? bp0 : bp0 + i[1] * bstride[1];
			for (i[2] = 0; i[2] < adim[2]; i[2]++)
			{
				float* const bp2 = bdim[2] == 1 ? bp1 : bp1 + i[2] * bstride[2];
				if (bdim[3] == 1)
					for (x = 0; x < adim[3]; x++)
						bp2[0] += ap1[x];
				else
					for (x = 0; x < adim[3]; x++)
						bp2[x] += ap1[x];
				ap1 += astride[2];
			}
		}
	}
}

static int _ccv_nnc_reduce_sum_forw(const ccv_nnc_cmd_t cmd, const ccv_nnc_hint_t hint, const int flags, ccv_nnc_tensor_t* const* const inputs, const int input_size, ccv_nnc_tensor_t* const* const outputs, const int output_size, ccv_nnc_stream_context_t* const stream_context)
{
	assert(input_size == 1);
	ccv_nnc_tensor_view_t* const a = (ccv_nnc_tensor_view_t*)inputs[0];
	ccv_nnc_tensor_view_t* const b = (ccv_nnc_tensor_view_t*)outputs[0];
	_ccv_nnc_reduce_sum_forw_cpu_ref(a, b);
	return CCV_NNC_EXEC_SUCCESS;
}

static int _ccv_nnc_reduce_sum_back(const ccv_nnc_cmd_t cmd, const ccv_nnc_hint_t hint, const int flags, ccv_nnc_tensor_t* const* const inputs, const int input_size, ccv_nnc_tensor_t* const* const outputs, const int output_size, ccv_nnc_stream_context_t* const stream_context)
{
	if (inputs[0] == 0)
	{
		_ccv_nnc_tensor_set_cpu_ref_f32((ccv_nnc_tensor_view_t*)outputs[0], 1);
		return CCV_NNC_EXEC_SUCCESS;
	}
	ccv_nnc_tensor_view_t* const a = (ccv_nnc_tensor_view_t*)outputs[0];
	ccv_nnc_tensor_view_t* const b = (ccv_nnc_tensor_view_t*)inputs[0];
	assert(ccv_nnc_tensor_nd(a->info.dim) <= CCV_NNC_MAX_DIM + 2);
	assert(ccv_nnc_tensor_nd(b->info.dim) <= CCV_NNC_MAX_DIM + 2);
	// Assuming this is float 32.
	int adim[CCV_NNC_MAX_DIM_ALLOC];
	int bdim[CCV_NNC_MAX_DIM_ALLOC];
	ccv_nnc_tensor_view_get_dim(a, adim);
	ccv_nnc_tensor_view_get_dim(b, bdim);
	int astride[CCV_NNC_MAX_DIM_ALLOC];
	int bstride[CCV_NNC_MAX_DIM_ALLOC];
	assert(CCV_NNC_MAX_DIM == 2); // Need to change this logic for CCV_NNC_MAX_DIM == other number.
	ccv_nnc_tensor_view_get_stride(a, astride);
	ccv_nnc_tensor_view_get_stride(b, bstride);
	int i[CCV_NNC_MAX_DIM + 2];
	int x;
	float* const ap = a->data.f32;
	float* const bp = b->data.f32;
	// Non-optimal case, need to do skip if needed.
	for (i[0] = 0; i[0] < adim[0]; i[0]++)
	{
		float* const ap0 = ap + i[0] * astride[0];
		float* const bp0 = bdim[0] == 1 ? bp : bp + i[0] * bstride[0];
		for (i[1] = 0; i[1] < adim[1]; i[1]++)
		{
			float* ap1 = ap0 + i[1] * astride[1];
			float* const bp1 = bdim[1] == 1 ? bp0 : bp0 + i[1] * bstride[1];
			for (i[2] = 0; i[2] < adim[2]; i[2]++)
			{
				float* const bp2 = bdim[2] == 1 ? bp1 : bp1 + i[2] * bstride[2];
				if (bdim[3] == 1)
					for (x = 0; x < adim[3]; x++)
						ap1[x] = bp2[0];
				else
					for (x = 0; x < adim[3]; x++)
						ap1[x] = bp2[x];
				ap1 += astride[2];
			}
		}
	}
	return CCV_NNC_EXEC_SUCCESS;
}

REGISTER_COMMAND_BACKEND(CCV_NNC_REDUCE_SUM_FORWARD, CCV_NNC_BACKEND_CPU_REF)(ccv_nnc_cmd_backend_registry_t* const registry)
{
	registry->tensor_formats = CCV_TENSOR_FORMAT_NHWC | CCV_TENSOR_FORMAT_NCHW | CCV_TENSOR_FORMAT_CHWN;
	registry->tensor_datatypes = CCV_32F;
	registry->tensor_memory = CCV_TENSOR_CPU_MEMORY;
	registry->algorithms = 1;
	registry->exec = _ccv_nnc_reduce_sum_forw;
}

REGISTER_COMMAND_BACKEND(CCV_NNC_REDUCE_SUM_BACKWARD, CCV_NNC_BACKEND_CPU_REF)(ccv_nnc_cmd_backend_registry_t* const registry)
{
	registry->tensor_formats = CCV_TENSOR_FORMAT_NHWC | CCV_TENSOR_FORMAT_NCHW | CCV_TENSOR_FORMAT_CHWN;
	registry->tensor_datatypes = CCV_32F;
	registry->tensor_memory = CCV_TENSOR_CPU_MEMORY;
	registry->algorithms = 1;
	registry->exec = _ccv_nnc_reduce_sum_back;
}
