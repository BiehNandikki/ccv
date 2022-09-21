#include "case.h"
#include "ccv_case.h"
#include "ccv_nnc_case.h"
#include <ccv.h>
#include <nnc/ccv_nnc.h>
#include <nnc/ccv_nnc_easy.h>
#include <3rdparty/dsfmt/dSFMT.h>

TEST_SETUP()
{
	ccv_nnc_init();
}

TEST_CASE("data transfer between different tensor views")
{
	ccv_nnc_tensor_t* a = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 56, 56, 128), 0);
	ccv_nnc_tensor_t* b = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 24, 32, 64), 0);
	ccv_nnc_cmd_t cmd = CMD_DATA_TRANSFER_FORWARD();
	int i;
	for (i = 0; i < 128 * 56 * 56; i++)
		a->data.f32[i] = i;
	// 6 values, manageable.
	ccv_nnc_tensor_view_t a_view = ccv_nnc_tensor_view(a, CPU_TENSOR_NHWC(32F, 2, 3, 4), DIM_ALLOC(4, 3, 2), DIM_ALLOC(56 * 128, 128, 1));
	ccv_nnc_tensor_view_t b_view = ccv_nnc_tensor_view(b, CPU_TENSOR_NHWC(32F, 2, 3, 4), DIM_ALLOC(0, 0, 0), DIM_ALLOC(32 * 64, 64, 1));
	memset(b->data.f32, 0, sizeof(float) * 64 * 32 * 24);
	ccv_nnc_cmd_exec(cmd, ccv_nnc_no_hint, 0, TENSOR_LIST((ccv_nnc_tensor_t*)&a_view), TENSOR_LIST((ccv_nnc_tensor_t*)&b_view), 0);
	ccv_nnc_tensor_t* c = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 24, 32, 64), 0);
	memset(c->data.f32, 0, sizeof(float) * 64 * 32 * 24);
	c->data.f32[0] = 128 * 56 * 4 + 128 * 3 + 2;
	c->data.f32[1] = 128 * 56 * 4 + 128 * 3 + 3;
	c->data.f32[2] = 128 * 56 * 4 + 128 * 3 + 4;
	c->data.f32[3] = 128 * 56 * 4 + 128 * 3 + 5;
	c->data.f32[64] = 128 * 56 * 4 + 128 * (3 + 1) + 2;
	c->data.f32[65] = 128 * 56 * 4 + 128 * (3 + 1) + 3;
	c->data.f32[66] = 128 * 56 * 4 + 128 * (3 + 1) + 4;
	c->data.f32[67] = 128 * 56 * 4 + 128 * (3 + 1) + 5;
	c->data.f32[128] = 128 * 56 * 4 + 128 * (3 + 2) + 2;
	c->data.f32[129] = 128 * 56 * 4 + 128 * (3 + 2) + 3;
	c->data.f32[130] = 128 * 56 * 4 + 128 * (3 + 2) + 4;
	c->data.f32[131] = 128 * 56 * 4 + 128 * (3 + 2) + 5;
	c->data.f32[64 * 32] = 128 * 56 * (4 + 1) + 128 * 3 + 2;
	c->data.f32[64 * 32 + 1] = 128 * 56 * (4 + 1) + 128 * 3 + 3;
	c->data.f32[64 * 32 + 2] = 128 * 56 * (4 + 1) + 128 * 3 + 4;
	c->data.f32[64 * 32 + 3] = 128 * 56 * (4 + 1) + 128 * 3 + 5;
	c->data.f32[64 * 32 + 64] = 128 * 56 * (4 + 1) + 128 * (3 + 1) + 2;
	c->data.f32[64 * 32 + 65] = 128 * 56 * (4 + 1) + 128 * (3 + 1) + 3;
	c->data.f32[64 * 32 + 66] = 128 * 56 * (4 + 1) + 128 * (3 + 1) + 4;
	c->data.f32[64 * 32 + 67] = 128 * 56 * (4 + 1) + 128 * (3 + 1) + 5;
	c->data.f32[64 * 32 + 128] = 128 * 56 * (4 + 1) + 128 * (3 + 2) + 2;
	c->data.f32[64 * 32 + 129] = 128 * 56 * (4 + 1) + 128 * (3 + 2) + 3;
	c->data.f32[64 * 32 + 130] = 128 * 56 * (4 + 1) + 128 * (3 + 2) + 4;
	c->data.f32[64 * 32 + 131] = 128 * 56 * (4 + 1) + 128 * (3 + 2) + 5;
	REQUIRE_TENSOR_EQ(b, c, "64x32x24 tensor should be exactly the same.");
	ccv_nnc_tensor_free(a);
	ccv_nnc_tensor_free(b);
	ccv_nnc_tensor_free(c);
}

TEST_CASE("format transform between NHWC and NCHW tensors")
{
	ccv_nnc_tensor_t* a = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 4, 3, 2), 0);
	ccv_nnc_tensor_t* b = ccv_nnc_tensor_new(0, CPU_TENSOR_NCHW(32F, 2, 4, 3), 0);
	ccv_nnc_cmd_t cmd = CMD_FORMAT_TRANSFORM_FORWARD();
	int i;
	for (i = 0; i < 2 * 3 * 4; i++)
		a->data.f32[i] = i;
	ccv_nnc_cmd_exec(cmd, ccv_nnc_no_hint, 0, TENSOR_LIST(a), TENSOR_LIST(b), 0);
	ccv_nnc_tensor_t* c = ccv_nnc_tensor_new(0, CPU_TENSOR_NCHW(32F, 2, 4, 3), 0);
	c->data.f32[0] = 0;
	c->data.f32[1] = 2;
	c->data.f32[2] = 4;
	c->data.f32[3] = 6;
	c->data.f32[4] = 8;
	c->data.f32[5] = 10;
	c->data.f32[6] = 12;
	c->data.f32[7] = 14;
	c->data.f32[8] = 16;
	c->data.f32[9] = 18;
	c->data.f32[10] = 20;
	c->data.f32[11] = 22;
	c->data.f32[12] = 1;
	c->data.f32[13] = 3;
	c->data.f32[14] = 5;
	c->data.f32[15] = 7;
	c->data.f32[16] = 9;
	c->data.f32[17] = 11;
	c->data.f32[18] = 13;
	c->data.f32[19] = 15;
	c->data.f32[20] = 17;
	c->data.f32[21] = 19;
	c->data.f32[22] = 21;
	c->data.f32[23] = 23;
	REQUIRE_TENSOR_EQ(b, c, "3x4x2 tensor should be exactly the same.");
	ccv_nnc_tensor_t* d = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 4, 3, 2), 0);
	ccv_nnc_cmd_exec(cmd, ccv_nnc_no_hint, 0, TENSOR_LIST(c), TENSOR_LIST(d), 0);
	REQUIRE_TENSOR_EQ(d, a, "2x3x4 tensor should be exactly the same.");
	ccv_nnc_tensor_free(a);
	ccv_nnc_tensor_free(b);
	ccv_nnc_tensor_free(c);
	ccv_nnc_tensor_free(d);
}

TEST_CASE("format transform between NHWC and NCHW tensor views")
{
	ccv_nnc_tensor_t* a = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 7, 6, 5), 0);
	ccv_nnc_tensor_view_t a_view = ccv_nnc_tensor_view(a, CPU_TENSOR_NHWC(32F, 4, 3, 2), DIM_ALLOC(3, 2, 1), DIM_ALLOC(6 * 5, 5, 1));
	memset(a->data.f32, 0, sizeof(float) * 5 * 6 * 7);
	ccv_nnc_tensor_t* b = ccv_nnc_tensor_new(0, CPU_TENSOR_NCHW(32F, 8, 10, 8), 0);
	memset(b->data.f32, 0, sizeof(float) * 8 * 10 * 8);
	ccv_nnc_tensor_view_t b_view = ccv_nnc_tensor_view(b, CPU_TENSOR_NCHW(32F, 2, 4, 3), DIM_ALLOC(0, 0, 0), DIM_ALLOC(10 * 8, 8, 1));
	ccv_nnc_cmd_t cmd = CMD_FORMAT_TRANSFORM_FORWARD();
	int i, j, k;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 3; j++)
			for (k = 0; k < 2; k++)
				a->data.f32[(i + 3) * 5 * 6 + (j + 2) * 5 + (k + 1)] = k + j * 2 + i * 3 * 2;
	ccv_nnc_cmd_exec(cmd, ccv_nnc_no_hint, 0, TENSOR_LIST((ccv_nnc_tensor_t*)&a_view), TENSOR_LIST((ccv_nnc_tensor_t*)&b_view), 0);
	ccv_nnc_tensor_t* c = ccv_nnc_tensor_new(0, CPU_TENSOR_NCHW(32F, 8, 10, 8), 0);
	memset(c->data.f32, 0, sizeof(float) * 8 * 10 * 8);
	c->data.f32[0] = 0;
	c->data.f32[1] = 2;
	c->data.f32[2] = 4;
	c->data.f32[8] = 6;
	c->data.f32[9] = 8;
	c->data.f32[10] = 10;
	c->data.f32[16] = 12;
	c->data.f32[17] = 14;
	c->data.f32[18] = 16;
	c->data.f32[24] = 18;
	c->data.f32[25] = 20;
	c->data.f32[26] = 22;
	c->data.f32[80] = 1;
	c->data.f32[81] = 3;
	c->data.f32[82] = 5;
	c->data.f32[88] = 7;
	c->data.f32[89] = 9;
	c->data.f32[90] = 11;
	c->data.f32[96] = 13;
	c->data.f32[97] = 15;
	c->data.f32[98] = 17;
	c->data.f32[104] = 19;
	c->data.f32[105] = 21;
	c->data.f32[106] = 23;
	REQUIRE_TENSOR_EQ(b, c, "3x4x2 tensor view should be exactly the same.");
	ccv_nnc_tensor_view_t c_view = ccv_nnc_tensor_view(c, CPU_TENSOR_NCHW(32F, 2, 4, 3), DIM_ALLOC(0, 0, 0), DIM_ALLOC(10 * 8, 8, 1));
	ccv_nnc_tensor_t* d = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 7, 6, 5), 0);
	memset(d->data.f32, 0, sizeof(float) * 5 * 6 * 7);
	ccv_nnc_tensor_view_t d_view = ccv_nnc_tensor_view(d, CPU_TENSOR_NHWC(32F, 4, 3, 2), DIM_ALLOC(3, 2, 1), DIM_ALLOC(6 * 5, 5, 1));
	ccv_nnc_cmd_exec(cmd, ccv_nnc_no_hint, 0, TENSOR_LIST((ccv_nnc_tensor_t*)&c_view), TENSOR_LIST((ccv_nnc_tensor_t*)&d_view), 0);
	REQUIRE_TENSOR_EQ(d, a, "2x3x4 tensor should be exactly the same.");
	ccv_nnc_tensor_free(a);
	ccv_nnc_tensor_free(b);
	ccv_nnc_tensor_free(c);
	ccv_nnc_tensor_free(d);
}

TEST_CASE("transpose two 4d tensor views")
{
	ccv_nnc_tensor_t* const a = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 7, 6, 5, 4), 0);
	memset(a->data.f32, 0, sizeof(float) * 7 * 6 * 5 * 4);
	ccv_nnc_tensor_view_t a_view = ccv_nnc_tensor_view(a, CPU_TENSOR_NHWC(32F, 4, 3, 2, 2), DIM_ALLOC(3, 2, 1, 0), DIM_ALLOC(6 * 5 * 4, 5 * 4, 4, 1));
	ccv_nnc_tensor_t* const b = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 8, 7, 6, 5), 0);
	memset(b->data.f32, 0, sizeof(float) * 8 * 7 * 6 * 5);
	ccv_nnc_tensor_view_t b_view = ccv_nnc_tensor_view(b, CPU_TENSOR_NHWC(32F, 4, 2, 2, 3), DIM_ALLOC(3, 2, 1, 0), DIM_ALLOC(7 * 6 * 5, 6 * 5, 5, 1));
	int i, j, k, l;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 3; j++)
			for (k = 0; k < 2; k++)
				for (l = 0; l < 2; l++)
					a->data.f32[(i + 3) * 6 * 5 * 4 + (j + 2) * 5 * 4 + (k + 1) * 4 + l] = i * 3 * 2 * 2 + j * 2 * 2 + k * 2 + l;
	ccv_nnc_cmd_exec(CMD_TRANSPOSE_FORWARD(1, 3), ccv_nnc_no_hint, 0, TENSOR_LIST((ccv_nnc_tensor_t*)&a_view), TENSOR_LIST((ccv_nnc_tensor_t*)&b_view), 0);
	ccv_nnc_tensor_t* c = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 8, 7, 6, 5), 0);
	memset(c->data.f32, 0, sizeof(float) * 8 * 7 * 6 * 5);
	for (i = 0; i < 4; i++)
		for (j = 0; j < 3; j++)
			for (k = 0; k < 2; k++)
				for (l = 0; l < 2; l++)
					c->data.f32[(i + 3) * 7 * 6 * 5 + (l + 2) * 6 * 5 + (k + 1) * 5 + j] = i * 3 * 2 * 2 + j * 2 * 2 + k * 2 + l;
	REQUIRE_TENSOR_EQ(b, c, "4x2x2x3 tensor view should be exactly the same.");
	ccv_nnc_tensor_view_t c_view = ccv_nnc_tensor_view(c, CPU_TENSOR_NHWC(32F, 4, 2, 2, 3), DIM_ALLOC(3, 2, 1, 0), DIM_ALLOC(7 * 6 * 5, 6 * 5, 5, 1));
	ccv_nnc_tensor_t* d = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 7, 6, 5, 4), 0);
	memset(d->data.f32, 0, sizeof(float) * 7 * 6 * 5 * 4);
	ccv_nnc_tensor_view_t d_view = ccv_nnc_tensor_view(d, CPU_TENSOR_NHWC(32F, 4, 3, 2, 2), DIM_ALLOC(3, 2, 1, 0), DIM_ALLOC(6 * 5 * 4, 5 * 4, 4, 1));
	ccv_nnc_cmd_exec(CMD_TRANSPOSE_FORWARD(1, 3), ccv_nnc_no_hint, 0, TENSOR_LIST((ccv_nnc_tensor_t*)&c_view), TENSOR_LIST((ccv_nnc_tensor_t*)&d_view), 0);
	REQUIRE_TENSOR_EQ(d, a, "4x3x2x2 tensor should be exactly the same.");
	ccv_nnc_tensor_free(a);
	ccv_nnc_tensor_free(b);
	ccv_nnc_tensor_free(c);
	ccv_nnc_tensor_free(d);
}

TEST_CASE("transpose two 3d tensor views")
{
	ccv_nnc_tensor_t* const a = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 6, 5, 4), 0);
	memset(a->data.f32, 0, sizeof(float) * 6 * 5 * 4);
	ccv_nnc_tensor_view_t a_view = ccv_nnc_tensor_view(a, CPU_TENSOR_NHWC(32F, 3, 2, 2), DIM_ALLOC(2, 1, 0), DIM_ALLOC(5 * 4, 4, 1));
	ccv_nnc_tensor_t* const b = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 7, 6, 5), 0);
	memset(b->data.f32, 0, sizeof(float) * 7 * 6 * 5);
	ccv_nnc_tensor_view_t b_view = ccv_nnc_tensor_view(b, CPU_TENSOR_NHWC(32F, 2, 2, 3), DIM_ALLOC(2, 1, 0), DIM_ALLOC(6 * 5, 5, 1));
	int j, k, l;
	for (j = 0; j < 3; j++)
		for (k = 0; k < 2; k++)
			for (l = 0; l < 2; l++)
				a->data.f32[(j + 2) * 5 * 4 + (k + 1) * 4 + l] = j * 2 * 2 + k * 2 + l;
	ccv_nnc_cmd_exec(CMD_TRANSPOSE_FORWARD(0, 2), ccv_nnc_no_hint, 0, TENSOR_LIST((ccv_nnc_tensor_t*)&a_view), TENSOR_LIST((ccv_nnc_tensor_t*)&b_view), 0);
	ccv_nnc_tensor_t* c = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 7, 6, 5), 0);
	memset(c->data.f32, 0, sizeof(float) * 7 * 6 * 5);
	for (j = 0; j < 3; j++)
		for (k = 0; k < 2; k++)
			for (l = 0; l < 2; l++)
				c->data.f32[(l + 2) * 6 * 5 + (k + 1) * 5 + j] = j * 2 * 2 + k * 2 + l;
	REQUIRE_TENSOR_EQ(b, c, "2x2x3 tensor view should be exactly the same.");
	ccv_nnc_tensor_view_t c_view = ccv_nnc_tensor_view(c, CPU_TENSOR_NHWC(32F, 2, 2, 3), DIM_ALLOC(2, 1, 0), DIM_ALLOC(6 * 5, 5, 1));
	ccv_nnc_tensor_t* d = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 6, 5, 4), 0);
	memset(d->data.f32, 0, sizeof(float) * 6 * 5 * 4);
	ccv_nnc_tensor_view_t d_view = ccv_nnc_tensor_view(d, CPU_TENSOR_NHWC(32F, 3, 2, 2), DIM_ALLOC(2, 1, 0), DIM_ALLOC(5 * 4, 4, 1));
	ccv_nnc_cmd_exec(CMD_TRANSPOSE_FORWARD(0, 2), ccv_nnc_no_hint, 0, TENSOR_LIST((ccv_nnc_tensor_t*)&c_view), TENSOR_LIST((ccv_nnc_tensor_t*)&d_view), 0);
	REQUIRE_TENSOR_EQ(d, a, "3x2x2 tensor should be exactly the same.");
	ccv_nnc_tensor_free(a);
	ccv_nnc_tensor_free(b);
	ccv_nnc_tensor_free(c);
	ccv_nnc_tensor_free(d);
}

TEST_CASE("masked fill forward a 3d tensor")
{
	ccv_nnc_tensor_t* const a = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 6, 5, 4), 0);
	int i, j;
	dsfmt_t dsfmt;
	dsfmt_init_gen_rand(&dsfmt, 0);
	for (i = 0; i < 6 * 5 * 4; i++)
		a->data.f32[i] = dsfmt_genrand_open_close(&dsfmt);
	ccv_nnc_tensor_t* const b = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32S, 5, 4), 0);
	for (i = 0; i < 5 * 4; i++)
		b->data.i32[i] = (i % 2 == 1) ? 0 : 1;
	ccv_nnc_tensor_t* const c = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 6, 5, 4), 0);
	ccv_nnc_cmd_exec(CMD_MASKED_FILL_FORWARD(0, -1e8), ccv_nnc_no_hint, 0, TENSOR_LIST(a, b), TENSOR_LIST(c), 0);
	ccv_nnc_tensor_t* const d = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 6, 5, 4), 0);
	for (i = 0; i < 6; i++)
		for (j = 0; j < 5 * 4; j++)
			d->data.f32[i * 5 * 4 + j] = (j % 2 == 1) ? -1e8 : a->data.f32[i * 5 * 4 + j];
	REQUIRE_TENSOR_EQ(d, c, "6x5x4 tensor should be exactly the same.");
	ccv_nnc_tensor_free(a);
	ccv_nnc_tensor_free(b);
	ccv_nnc_tensor_free(c);
	ccv_nnc_tensor_free(d);
}

TEST_CASE("masked fill backward a 3d tensor")
{
	ccv_nnc_tensor_t* const a = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 6, 5, 4), 0);
	int i, j;
	dsfmt_t dsfmt;
	dsfmt_init_gen_rand(&dsfmt, 0);
	for (i = 0; i < 6 * 5 * 4; i++)
		a->data.f32[i] = dsfmt_genrand_open_close(&dsfmt);
	ccv_nnc_tensor_t* const b = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32S, 5, 4), 0);
	for (i = 0; i < 5 * 4; i++)
		b->data.i32[i] = (i % 2 == 1) ? 0 : 1;
	ccv_nnc_tensor_t* const c = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 6, 5, 4), 0);
	ccv_nnc_cmd_exec(CMD_MASKED_FILL_BACKWARD(0, -1e8), ccv_nnc_no_hint, 0, TENSOR_LIST(a, 0, b), TENSOR_LIST(c), 0);
	ccv_nnc_tensor_t* const d = ccv_nnc_tensor_new(0, CPU_TENSOR_NHWC(32F, 6, 5, 4), 0);
	for (i = 0; i < 6; i++)
		for (j = 0; j < 5 * 4; j++)
			d->data.f32[i * 5 * 4 + j] = (j % 2 == 1) ? 0 : a->data.f32[i * 5 * 4 + j];
	REQUIRE_TENSOR_EQ(d, c, "6x5x4 tensor should be exactly the same.");
	ccv_nnc_tensor_free(a);
	ccv_nnc_tensor_free(b);
	ccv_nnc_tensor_free(c);
	ccv_nnc_tensor_free(d);
}

#include "case_main.h"
