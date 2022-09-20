#include "ccv.h"
#include "case.h"
#include "ccv_case.h"
#include "ccv_nnc_case.h"
#include "nnc/ccv_nnc.h"
#include "nnc/ccv_nnc_easy.h"
#include "3rdparty/sqlite3/sqlite3.h"
#include "3rdparty/dsfmt/dSFMT.h"

TEST_SETUP()
{
	ccv_nnc_init();
}

TEST_CASE("zero out a tensor")
{
	const ccv_nnc_tensor_param_t params = {
		.type = CCV_TENSOR_CPU_MEMORY,
		.format = CCV_TENSOR_FORMAT_NHWC,
		.datatype = CCV_32F,
		.dim = {
			10, 20, 30, 4, 5, 6,
		},
	};
	ccv_nnc_tensor_t* tensor = ccv_nnc_tensor_new(0, params, 0);
	int i;
	for (i = 0; i < 10 * 20 * 30 * 4 * 5 * 6; i++)
		tensor->data.f32[i] = 1;
	ccv_nnc_tensor_zero(tensor);
	for (i = 0; i < 10 * 20 * 30 * 4 * 5 * 6; i++)
		REQUIRE_EQ(0, tensor->data.f32[i], "should be zero'ed at %d", i);
	ccv_nnc_tensor_free(tensor);
}

TEST_CASE("zero out a tensor view")
{
	const ccv_nnc_tensor_param_t params = {
		.type = CCV_TENSOR_CPU_MEMORY,
		.format = CCV_TENSOR_FORMAT_NHWC,
		.datatype = CCV_32F,
		.dim = {
			10, 20, 30, 4, 5, 6,
		},
	};
	ccv_nnc_tensor_t* a_tensor = ccv_nnc_tensor_new(0, params, 0);
	int c;
	for (c = 0; c < 10 * 20 * 30 * 4 * 5 * 6; c++)
		a_tensor->data.f32[c] = 1;
	int ofs[CCV_NNC_MAX_DIM_ALLOC] = {
		1, 2, 5, 1, 1, 1,
	};
	const ccv_nnc_tensor_param_t new_params = {
		.type = CCV_TENSOR_CPU_MEMORY,
		.format = CCV_TENSOR_FORMAT_NHWC,
		.datatype = CCV_32F,
		.dim = {
			8, 12, 15, 2, 3, 4,
		},
	};
	ccv_nnc_tensor_view_t a_tensor_view = ccv_nnc_tensor_view(a_tensor, new_params, ofs, a_tensor->info.dim);
	ccv_nnc_tensor_zero(&a_tensor_view);
	ccv_nnc_tensor_t* b_tensor = ccv_nnc_tensor_new(0, params, 0);
	for (c = 0; c < 10 * 20 * 30 * 4 * 5 * 6; c++)
		b_tensor->data.f32[c] = 1;
	ccv_nnc_tensor_view_t b_tensor_view = ccv_nnc_tensor_view(b_tensor, new_params, ofs, b_tensor->info.dim);
	int i[6];
	float* tvp[6];
	tvp[5] = b_tensor_view.data.f32;
	for (i[5] = 0; i[5] < b_tensor_view.info.dim[0]; i[5]++)
	{
		tvp[4] = tvp[5];
		for (i[4] = 0; i[4] < b_tensor_view.info.dim[1]; i[4]++)
		{
			tvp[3] = tvp[4];
			for (i[3] = 0; i[3] < b_tensor_view.info.dim[2]; i[3]++)
			{
				tvp[2] = tvp[3];
				for (i[2] = 0; i[2] < b_tensor_view.info.dim[3]; i[2]++)
				{
					tvp[1] = tvp[2];
					for (i[1] = 0; i[1] < b_tensor_view.info.dim[4]; i[1]++)
					{
						tvp[0] = tvp[1];
						for (i[0] = 0; i[0] < b_tensor_view.info.dim[5]; i[0]++)
						{
							tvp[0][i[0]] = 0;
						}
						tvp[1] += b_tensor_view.stride[4];
					}
					tvp[2] += b_tensor_view.stride[3];
				}
				tvp[3] += b_tensor_view.stride[2];
			}
			tvp[4] += b_tensor_view.stride[1];
		}
		tvp[5] += b_tensor_view.stride[0];
	}
	REQUIRE_TENSOR_EQ(a_tensor, b_tensor, "zero'ed tensor view should be equal");
	ccv_nnc_tensor_free(a_tensor);
	ccv_nnc_tensor_free(b_tensor);
}

TEST_CASE("hint tensor")
{
	ccv_nnc_tensor_param_t a = CPU_TENSOR_NHWC(32F, 234, 128, 3);
	ccv_nnc_hint_t hint = {
		.border = {
			.begin = {1, 1},
			.end = {1, 2}
		},
		.stride = {
			.dim = {8, 7}
		}
	};
	ccv_nnc_cmd_t cmd = CMD_CONVOLUTION_FORWARD(1, 128, 4, 5, 3);
	ccv_nnc_tensor_param_t b;
	ccv_nnc_tensor_param_t w = CPU_TENSOR_NHWC(32F, 128, 4, 5, 3);
	ccv_nnc_tensor_param_t bias = CPU_TENSOR_NHWC(32F, 128);
	ccv_nnc_hint_tensor_auto(cmd, TENSOR_PARAM_LIST(a, w, bias), hint, &b, 1);
	REQUIRE_EQ(b.dim[0], 30, "height should be 30");
	REQUIRE_EQ(b.dim[1], 19, "width should be 19");
	REQUIRE_EQ(b.dim[2], 128, "channel should be the convolution filter count");
}

TEST_CASE("tensor persistence")
{
	sqlite3* handle;
	sqlite3_open("tensors.sqlite3", &handle);
	ccv_nnc_tensor_t* const tensor = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 10, 20, 30), 0);
	int i;
	dsfmt_t dsfmt;
	dsfmt_init_gen_rand(&dsfmt, 1);
	for (i = 0; i < 10 * 20 * 30; i++)
		tensor->data.f32[i] = dsfmt_genrand_open_close(&dsfmt) * 2 - 1;
	ccv_nnc_tensor_write(tensor, handle, "x");
	sqlite3_close(handle);
	handle = 0;
	sqlite3_open("tensors.sqlite3", &handle);
	ccv_nnc_tensor_t* tensor1 = 0;
	ccv_nnc_tensor_read(handle, "x", &tensor1);
	ccv_nnc_tensor_t* tensor2 = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 10), 0);
	ccv_nnc_tensor_read(handle, "x", &tensor2);
	sqlite3_close(handle);
	REQUIRE_TENSOR_EQ(tensor1, tensor, "the first tensor should equal to the second");
	REQUIRE_ARRAY_EQ_WITH_TOLERANCE(float, tensor2->data.f32, tensor->data.f32, 10, 1e-5, "the first 10 element should be equal");
	REQUIRE(ccv_nnc_tensor_nd(tensor2->info.dim) == 1, "should be 1-d tensor");
	REQUIRE_EQ(tensor2->info.dim[0], 10, "should be 1-d tensor with 10-element");
	ccv_nnc_tensor_free(tensor1);
	ccv_nnc_tensor_free(tensor2);
	ccv_nnc_tensor_free(tensor);
}

TEST_CASE("resize tensor")
{
	ccv_nnc_tensor_t* tensor = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 12, 12, 3), 0);
	int i;
	for (i = 0; i < 12 * 12 * 3; i++)
		tensor->data.f32[i] = i;
	tensor = ccv_nnc_tensor_resize(tensor, CPU_TENSOR_NHWC(32F, 23, 23, 3));
	for (i = 12 * 12 * 3; i < 23 * 23 * 3; i++)
		tensor->data.f32[i] = i;
	ccv_nnc_tensor_t* b = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 23, 23, 3), 0);
	for (i = 0; i < 23 * 23 * 3; i++)
		b->data.f32[i] = i;
	REQUIRE_TENSOR_EQ(tensor, b, "should retain the content when resize a tensor");
	ccv_nnc_tensor_free(tensor);
	ccv_nnc_tensor_free(b);
}

#include "case_main.h"
