#include "ccv_nnc.h"
#include "ccv_nnc_easy.h"
#include "ccv_nnc_internal.h"
#include "ccv_internal.h"
#include "_ccv_nnc_micro.h"
#include "3rdparty/khash/khash.h"

#define MICRO_ID_TO_INT(x) (((x).id << 8) | ((x).d))
KHASH_MAP_INIT_INT(ccv_nnc_axis_id_group, int)

static int _ccv_nnc_same_index_term(const ccv_nnc_micro_loop_index_term_t a_index, const ccv_nnc_micro_loop_index_term_t b_index, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	if (a_index.type != b_index.type)
		return 0;
	const int type = a_index.type;
	switch (type)
	{
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL:
			return a_index.immediate_value == b_index.immediate_value;
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_ID:
			if (a_index.id.type != b_index.id.type)
				return 0;
			// Check within the axis_id_groups to see if there is a match, if there is no match, we can proceed (to use the group table again to check).
			if (axis_id_groups && a_index.id.type == CCV_NNC_MICRO_AXIS_SIZE_ID)
			{
				ccv_nnc_micro_id_t a_id = a_index.id;
				while (groups && groups[a_id.id] != a_id.id)
					a_id.id = groups[a_id.id];
				int a_root = MICRO_ID_TO_INT(a_id);
				khiter_t k;
				for (;;) {
					k = kh_get(ccv_nnc_axis_id_group, axis_id_groups, a_root);
					if (k == kh_end(axis_id_groups))
						break;
					a_root = kh_val(axis_id_groups, k);
				}
				ccv_nnc_micro_id_t b_id = b_index.id;
				while (groups && groups[b_id.id] != b_id.id)
					b_id.id = groups[b_id.id];
				int b_root = MICRO_ID_TO_INT(b_id);
				for (;;) {
					k = kh_get(ccv_nnc_axis_id_group, axis_id_groups, b_root);
					if (k == kh_end(axis_id_groups))
						break;
					b_root = kh_val(axis_id_groups, k);
				}
				if (a_root == b_root)
					return 1;
			}
			if (groups && (a_index.id.type == CCV_NNC_MICRO_AXIS_SIZE_ID || a_index.id.type == CCV_NNC_MICRO_TENSOR_ID))
			{
				if (a_index.id.d != b_index.id.d)
					return 0;
				switch (a_index.id.type)
				{
					case CCV_NNC_MICRO_TENSOR_ID:
					case CCV_NNC_MICRO_AXIS_SIZE_ID: {
						// Find their group identifier and then compare.
						int a_root = groups[a_index.id.id];
						while (groups[a_root] != a_root)
							a_root = groups[a_root];
						int b_root = groups[b_index.id.id];
						while (groups[b_root] != b_root)
							b_root = groups[b_root];
						return a_root == b_root;
					}
				}
				return a_index.id.id == b_index.id.id;
			} else
				return (a_index.id.d == b_index.id.d && a_index.id.id == b_index.id.id);
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY: {
			return a_index.binary->op == b_index.binary->op && _ccv_nnc_same_index_term(a_index.binary->left, b_index.binary->left, groups, axis_id_groups) && _ccv_nnc_same_index_term(a_index.binary->right, b_index.binary->right, groups, axis_id_groups);
		}
	}
	return 0;
}

static int _ccv_nnc_same_shape(const ccv_nnc_micro_loop_index_term_t* const a_shape, const ccv_nnc_micro_loop_index_term_t* const b_shape, const int dimensions)
{
	int i;
	for (i = 0; i < dimensions; i++)
		if (!_ccv_nnc_same_index_term(a_shape[i], b_shape[i], 0, 0))
			return 0;
	return 1;
}

static int _ccv_nnc_same_loop(const ccv_nnc_micro_loop_block_t* const left_block, const ccv_nnc_micro_loop_block_t* const right_block, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups, int* const left_loop_idx, int* const right_loop_idx)
{
	assert(left_block->loop_count > 0);
	assert(right_block->loop_count > 0);
	int i, j;
	int left_right_link[left_block->loop_count];
	int right_left_link[right_block->loop_count];
	enum {
		ONE = -2,
		UNASSIGNED = -1,
	};
	for (i = 0; i < left_block->loop_count; i++)
		if (left_block->loops[i].start_index.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL && left_block->loops[i].start_index.immediate_value == 0 &&
			left_block->loops[i].end_index.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL && left_block->loops[i].end_index.immediate_value == 1)
			left_right_link[i] = ONE;
		else
			left_right_link[i] = UNASSIGNED;
	for (i = 0; i < right_block->loop_count; i++)
		if (right_block->loops[i].start_index.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL && right_block->loops[i].start_index.immediate_value == 0 &&
			right_block->loops[i].end_index.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL && right_block->loops[i].end_index.immediate_value == 1)
			right_left_link[i] = ONE;
		else
			right_left_link[i] = UNASSIGNED;
	for (i = 0; i < left_block->loop_count; i++) // Find corresponding loop on the right.
	{
		if (left_right_link[i] != UNASSIGNED)
			continue;
		int flag = UNASSIGNED;
		for (j = 0; j < right_block->loop_count && flag == UNASSIGNED; j++)
		{
			if (right_left_link[j] != UNASSIGNED)
				continue;
			if (_ccv_nnc_same_index_term(left_block->loops[i].start_index, right_block->loops[j].start_index, groups, axis_id_groups) && 
				_ccv_nnc_same_index_term(left_block->loops[i].end_index, right_block->loops[j].end_index, groups, axis_id_groups))
				flag = j;
		}
		if (flag != UNASSIGNED)
		{
			left_right_link[i] = flag;
			right_left_link[flag] = i;
		}
	}
	// If still have unmatched, they don't share the same loop.
	for (i = 0; i < left_block->loop_count; i++)
		if (left_right_link[i] == UNASSIGNED)
			return 0;
	for (i = 0; i < right_block->loop_count; i++)
		if (right_left_link[i] == UNASSIGNED)
			return 0;
	// I don't want to deal with constant loop, hence, if other than the outer-most is a constant loop (0..<1),
	// we cannot merge.
	for (i = 1; i < left_block->loop_count; i++)
		if (left_right_link[i] == ONE)
			return 0;
	for (i = 1; i < right_block->loop_count; i++)
		if (right_left_link[i] == ONE)
			return 0;
	assert((left_block->loop_count == right_block->loop_count) ||
			(left_block->loop_count == right_block->loop_count + 1) ||
			(left_block->loop_count + 1 == right_block->loop_count));
	// The loop matches, but the ordering probably doesn't. We reorder loop based on statements.
	// Hence, two loops can only merge if using the statements as a pivot point and they can still
	// match things before / after the statement.
	// If both have statements, check if order preserving within the statement loop (we can be fancier
	// and recursively call this while using statement as pivoting point, but that is too much to my taste).
	const int left_start_idx = left_right_link[0] == ONE ? 1 : 0;
	const int right_start_idx = right_left_link[0] == ONE ? 1 : 0;
	for (i = 0; i < left_block->loop_count; i++)
		left_loop_idx[i] = UNASSIGNED;
	for (i = 0; i < right_block->loop_count; i++)
		right_loop_idx[i] = UNASSIGNED;
	if (left_start_idx == 1)
		left_loop_idx[0] = 0; // Assign their index.
	if (right_start_idx == 0)
		right_loop_idx[0] = 0; // Assign their index.
	const int end_idx = left_right_link[0] == ONE && right_left_link[0] == ONE ? left_block->loop_count - 1 : ccv_min(left_block->loop_count, right_block->loop_count);
	int pivot_idx = end_idx;
	int k;
	for (i = end_idx - 1; i >= 0; i--)
	{
		if (left_block->loops[i + left_start_idx].statement_count > 0)
		{
			for (j = i + 1, k = i + 1; j < end_idx; j++)
				if (left_loop_idx[j + left_start_idx] == UNASSIGNED)
				{
					left_loop_idx[j + left_start_idx] = k + left_start_idx;
					// If the right one can be referenced pass previous pivot_idx, it is not right.
					if (left_right_link[j + left_start_idx] >= pivot_idx + right_start_idx)
						return 0;
					right_loop_idx[left_right_link[j + left_start_idx]] = k + right_start_idx;
					++k;
					if (k > pivot_idx)
						return 0;
				}
			assert(k == pivot_idx);
			pivot_idx = i + 1;
		}
		if (right_block->loops[i + right_start_idx].statement_count > 0)
		{
			for (j = i + 1, k = i + 1; j < end_idx; j++)
				if (right_loop_idx[j + left_start_idx] == UNASSIGNED)
				{
					right_loop_idx[j + right_start_idx] = k + right_start_idx;
					// If the left one can be referenced pass previous pivot_idx, it is not right.
					if (right_left_link[j + right_start_idx] >= pivot_idx + left_start_idx)
						return 0;
					left_loop_idx[right_left_link[j + right_start_idx]] = k + left_start_idx;
					++k;
					if (k > pivot_idx)
						return 0;
				}
			assert(k == pivot_idx);
			pivot_idx = i + 1;
		}
	}
	if (end_idx == 0)
		return 1;
	// Finally, to distribute the rest.
	for (j = 0, k = 0; j < end_idx; j++)
	{
		if (left_loop_idx[j + left_start_idx] == UNASSIGNED)
		{
			left_loop_idx[j + left_start_idx] = k + left_start_idx;
			// If the right one can be referenced pass previous pivot_idx, it is not right.
			if (left_right_link[j + left_start_idx] >= pivot_idx + right_start_idx)
				return 0;
			right_loop_idx[left_right_link[j + left_start_idx]] = k + right_start_idx;
			++k;
			if (k > pivot_idx)
				return 0;
		}
	}
	assert(k == pivot_idx);
	return 1;
}

static void _ccv_nnc_loop_order_by(ccv_nnc_micro_loop_block_t* const block, int* const loop_idx, ccv_nnc_micro_loop_t* const loops)
{
	int i;
	for (i = 0; i < block->loop_count; i++)
		if (loop_idx[i] >= 0)
			loops[loop_idx[i]] = block->loops[i];
		else
			loops[i] = block->loops[i];
	for (i = 0; i < block->loop_count; i++)
	{
		// Essentially, we don't need to move statements, loop-carried variables, just the loop id and the start / end index.
		block->loops[i].id = loops[i].id;
		block->loops[i].start_index = loops[i].start_index;
		block->loops[i].end_index = loops[i].end_index;
	}
}

static void _ccv_nnc_expression_rename_carrieds(ccv_nnc_micro_loop_expression_t* const expression, const int start_idx)
{
	switch (expression->type)
	{
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_ID:
			assert(expression->id.type == CCV_NNC_MICRO_LOOP_CARRIED_ID);
			expression->id.id += start_idx;
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_TERNAY:
			_ccv_nnc_expression_rename_carrieds(expression->ternary.pivot, start_idx);
			_ccv_nnc_expression_rename_carrieds(expression->ternary.left, start_idx);
			_ccv_nnc_expression_rename_carrieds(expression->ternary.right, start_idx);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_BINARY:
			_ccv_nnc_expression_rename_carrieds(expression->binary.left, start_idx);
			_ccv_nnc_expression_rename_carrieds(expression->binary.right, start_idx);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_UNARY:
			_ccv_nnc_expression_rename_carrieds(expression->unary.x, start_idx);
			break;
		// We don't need to care about other expressions because loop-carried variable cannot participate these operations.
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_VAR:
			break;
	}
}

static void _ccv_nnc_loop_rename_carrieds(ccv_nnc_micro_loop_block_t* const block, const int start_idx)
{
	int i, j;
	const int loop_count = block->loop_count;
	ccv_nnc_micro_loop_t* const loops = block->loops;
	for (i = 0; i < loop_count; i++)
	{
		for (j = 0; j < loops[i].carried_count; j++)
			loops[i].carrieds[j].id.id += start_idx;
		for (j = 0; j < loops[i].statement_count; j++)
			switch (loops[i].statements[j].type)
			{
				case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_ASSIGNMENT:
					_ccv_nnc_expression_rename_carrieds(&loops[i].statements[j].compound_assignment.rvalue, start_idx);
					break;
				case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_COMPOUND_ASSIGNMENT:
					if (loops[i].statements[j].compound_assignment.lvalue.type == CCV_NNC_MICRO_LOOP_EXPR_TYPE_ID)
					{
						assert(loops[i].statements[j].compound_assignment.lvalue.id.type == CCV_NNC_MICRO_LOOP_CARRIED_ID);
						loops[i].statements[j].compound_assignment.lvalue.id.id += start_idx;
					}
					_ccv_nnc_expression_rename_carrieds(&loops[i].statements[j].compound_assignment.rvalue, start_idx);
					break;
			}
	}
}

static int _ccv_nnc_only_var_in_expression(const int id, const ccv_nnc_micro_loop_variable_t var, const ccv_nnc_micro_loop_expression_t* const expression, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	switch (expression->type)
	{
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_VAR:
			if (expression->variable.id.type == CCV_NNC_MICRO_TENSOR_ID && expression->variable.id.id == id)
			{
				if (var.index_count != expression->variable.index_count)
					return 2;
				int i;
				for (i = 0; i < var.index_count; i++)
					if (!_ccv_nnc_same_index_term(var.index[i], expression->variable.index[i], groups, axis_id_groups))
						return 2;
				return 1;
			} else
				return 0;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_TERNAY: {
			const int pivot = _ccv_nnc_only_var_in_expression(id, var, expression->ternary.pivot, groups, axis_id_groups);
			const int left = _ccv_nnc_only_var_in_expression(id, var, expression->ternary.left, groups, axis_id_groups);
			const int right = _ccv_nnc_only_var_in_expression(id, var, expression->ternary.right, groups, axis_id_groups);
			if (pivot == 2 || left == 2 || right == 2)
				return 2;
			return (pivot || left || right);
		}
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_BINARY: {
			const int left = _ccv_nnc_only_var_in_expression(id, var, expression->binary.left, groups, axis_id_groups);
			const int right = _ccv_nnc_only_var_in_expression(id, var, expression->binary.right, groups, axis_id_groups);
			if (left == 2 || right == 2)
				return 2;
			return (left || right);
		}
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_UNARY:
			return _ccv_nnc_only_var_in_expression(id, var, expression->unary.x, groups, axis_id_groups);
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_ID:
			assert(expression->id.type == CCV_NNC_MICRO_LOOP_CARRIED_ID);
			return 0;
	}
	return 0;
}

static int _ccv_nnc_only_var_in_rvalue(const int id, const ccv_nnc_micro_loop_variable_t var, const ccv_nnc_micro_loop_statement_t statement, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	switch (statement.type)
	{
		case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_ASSIGNMENT:
			return _ccv_nnc_only_var_in_expression(id, var, &statement.assignment.rvalue, groups, axis_id_groups);
		case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_COMPOUND_ASSIGNMENT:
			return _ccv_nnc_only_var_in_expression(id, var, &statement.compound_assignment.rvalue, groups, axis_id_groups);
	}
	return 0;
}

static ccv_nnc_micro_loop_expression_t _ccv_nnc_expression_deep_copy(const ccv_nnc_micro_loop_expression_t* const expression)
{
	switch (expression->type)
	{
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_TERNAY: {
			ccv_nnc_micro_loop_expression_t copy = *expression;
			copy.ternary.pivot = (ccv_nnc_micro_loop_expression_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_expression_t));
			*copy.ternary.pivot = _ccv_nnc_expression_deep_copy(expression->ternary.pivot);
			copy.ternary.left = (ccv_nnc_micro_loop_expression_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_expression_t));
			*copy.ternary.left = _ccv_nnc_expression_deep_copy(expression->ternary.left);
			copy.ternary.right = (ccv_nnc_micro_loop_expression_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_expression_t));
			*copy.ternary.right = _ccv_nnc_expression_deep_copy(expression->ternary.right);
			return copy;
		}
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_BINARY: {
			ccv_nnc_micro_loop_expression_t copy = *expression;
			copy.binary.left = (ccv_nnc_micro_loop_expression_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_expression_t));
			*copy.binary.left = _ccv_nnc_expression_deep_copy(expression->binary.left);
			copy.binary.right = (ccv_nnc_micro_loop_expression_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_expression_t));
			*copy.binary.right = _ccv_nnc_expression_deep_copy(expression->binary.right);
			return copy;
		}
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_UNARY: {
			ccv_nnc_micro_loop_expression_t copy = *expression;
			copy.unary.x = (ccv_nnc_micro_loop_expression_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_expression_t));
			*copy.unary.x = _ccv_nnc_expression_deep_copy(expression->unary.x);
			return copy;
		}
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_VAR: {
			ccv_nnc_micro_loop_expression_t copy = *expression;
			int i;
			for (i = 0; i < copy.variable.index_count; i++)
				copy.variable.index[i] = ccv_nnc_micro_loop_index_deep_copy(&copy.variable.index[i]);
			return copy;
		}
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_ID:
			return *expression;
	}
	return *expression;
}

static void _ccv_nnc_replacing_id_in_expression(ccv_nnc_micro_loop_expression_t* const expression, const int id, ccv_nnc_micro_loop_expression_t rvalue, int* const count)
{
	switch (expression->type)
	{
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_VAR:
			if (expression->variable.id.type == CCV_NNC_MICRO_TENSOR_ID && expression->variable.id.id == id)
			{
				ccv_nnc_micro_loop_variable_free(&expression->variable);
				if (*count == 0) // First time, just assign to expression.
					*expression = rvalue;
				else // Otherwise, need to make deep copy of it.
					*expression = _ccv_nnc_expression_deep_copy(&rvalue);
				++(*count);
			}
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_TERNAY:
			_ccv_nnc_replacing_id_in_expression(expression->ternary.pivot, id, rvalue, count);
			_ccv_nnc_replacing_id_in_expression(expression->ternary.left, id, rvalue, count);
			_ccv_nnc_replacing_id_in_expression(expression->ternary.right, id, rvalue, count);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_BINARY:
			_ccv_nnc_replacing_id_in_expression(expression->binary.left, id, rvalue, count);
			_ccv_nnc_replacing_id_in_expression(expression->binary.right, id, rvalue, count);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_UNARY:
			_ccv_nnc_replacing_id_in_expression(expression->unary.x, id, rvalue, count);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_ID:
			assert(expression->id.type == CCV_NNC_MICRO_LOOP_CARRIED_ID);
			break;
	}
}

static void _ccv_nnc_replacing_id_in_rvalue(ccv_nnc_micro_loop_statement_t* const statement, const int id, ccv_nnc_micro_loop_expression_t rvalue, int* const count)
{
	switch (statement->type)
	{
		case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_ASSIGNMENT:
			_ccv_nnc_replacing_id_in_expression(&statement->assignment.rvalue, id, rvalue, count);
			break;
		case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_COMPOUND_ASSIGNMENT:
			// Not going to be in lvalue (which is the carried variable only).
			_ccv_nnc_replacing_id_in_expression(&statement->compound_assignment.rvalue, id, rvalue, count);
			break;
	}
}

typedef struct {
	int flag;
	int merge_to;
	ccv_array_t* writes;
	ccv_array_t* reads;
} ccv_nnc_micro_loop_block_dependency_t;

typedef struct {
	int flag;
	ccv_array_t* writes;
	ccv_array_t* reads;
} ccv_nnc_micro_tensor_dependency_t;

static void _ccv_nnc_micro_block_dependencies_from_rvalue(const ccv_nnc_micro_loop_expression_t* const rvalue, const int i, ccv_nnc_micro_loop_block_dependency_t* const block_dependencies, ccv_nnc_micro_tensor_dependency_t* const tensor_dependencies)
{
	switch (rvalue->type)
	{
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_VAR:
			if (rvalue->variable.id.type == CCV_NNC_MICRO_TENSOR_ID)
			{
				if (!block_dependencies[i].reads)
					block_dependencies[i].reads = ccv_array_new(sizeof(int), 1, 0);
				const int id = rvalue->variable.id.id;
				ccv_array_add_unique_int(block_dependencies[i].reads, id);
				if (!tensor_dependencies[id].reads)
					tensor_dependencies[id].reads = ccv_array_new(sizeof(int), 1, 0);
				ccv_array_add_unique_int(tensor_dependencies[id].reads, i);
			}
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_TERNAY:
			_ccv_nnc_micro_block_dependencies_from_rvalue(rvalue->ternary.pivot, i, block_dependencies, tensor_dependencies);
			_ccv_nnc_micro_block_dependencies_from_rvalue(rvalue->ternary.left, i, block_dependencies, tensor_dependencies);
			_ccv_nnc_micro_block_dependencies_from_rvalue(rvalue->ternary.right, i, block_dependencies, tensor_dependencies);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_BINARY:
			_ccv_nnc_micro_block_dependencies_from_rvalue(rvalue->binary.left, i, block_dependencies, tensor_dependencies);
			_ccv_nnc_micro_block_dependencies_from_rvalue(rvalue->binary.right, i, block_dependencies, tensor_dependencies);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_UNARY:
			_ccv_nnc_micro_block_dependencies_from_rvalue(rvalue->unary.x, i, block_dependencies, tensor_dependencies);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_ID:
			assert(rvalue->id.type == CCV_NNC_MICRO_LOOP_CARRIED_ID);
			break;
	}
}

static void _ccv_nnc_micro_block_dependencies(const ccv_nnc_micro_loop_block_t* const blocks, const int block_size, const int var_count, ccv_nnc_micro_loop_block_dependency_t** const block_dependencies_ref, ccv_nnc_micro_tensor_dependency_t** const tensor_dependencies_ref)
{
	ccv_nnc_micro_loop_block_dependency_t* const block_dependencies = (ccv_nnc_micro_loop_block_dependency_t*)cccalloc(block_size, sizeof(ccv_nnc_micro_loop_block_dependency_t));
	ccv_nnc_micro_tensor_dependency_t* const tensor_dependencies = (ccv_nnc_micro_tensor_dependency_t*)cccalloc(var_count, sizeof(ccv_nnc_micro_tensor_dependency_t));
	int i, j, k;
	for (i = 0; i < block_size; i++)
	{
		block_dependencies[i].merge_to = i;
		const ccv_nnc_micro_loop_t* const loops = blocks[i].loops;
		const int loop_count = blocks[i].loop_count;
		for (j = 0; j < loop_count; j++)
		{
			const ccv_nnc_micro_loop_statement_t* const statements = loops[j].statements;
			const int statement_count = loops[j].statement_count;
			for (k = 0; k < statement_count; k++)
				switch (statements[k].type)
				{
					case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_ASSIGNMENT: {
						assert(statements[k].assignment.lvalue.id.type == CCV_NNC_MICRO_TENSOR_ID);
						const int id = statements[k].assignment.lvalue.id.id;
						if (!block_dependencies[i].writes)
							block_dependencies[i].writes = ccv_array_new(sizeof(int), 1, 0);
						ccv_array_add_unique_int(block_dependencies[i].writes, id);
						if (!tensor_dependencies[id].writes)
							tensor_dependencies[id].writes = ccv_array_new(sizeof(int), 1, 0);
						ccv_array_add_unique_int(tensor_dependencies[id].writes, i);
						_ccv_nnc_micro_block_dependencies_from_rvalue(&statements[k].assignment.rvalue, i, block_dependencies, tensor_dependencies);
						break;
					}
					case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_COMPOUND_ASSIGNMENT: {
						if (statements[k].compound_assignment.lvalue.type == CCV_NNC_MICRO_LOOP_EXPR_TYPE_VAR)
						{
							assert(statements[k].compound_assignment.lvalue.id.type == CCV_NNC_MICRO_TENSOR_ID);
							const int id = statements[k].compound_assignment.lvalue.id.id;
							if (!block_dependencies[i].writes)
								block_dependencies[i].writes = ccv_array_new(sizeof(int), 1, 0);
							ccv_array_add_unique_int(block_dependencies[i].writes, id);
							if (!tensor_dependencies[id].writes)
								tensor_dependencies[id].writes = ccv_array_new(sizeof(int), 1, 0);
							ccv_array_add_unique_int(tensor_dependencies[id].writes, i);
							if (!block_dependencies[i].reads)
								block_dependencies[i].reads = ccv_array_new(sizeof(int), 1, 0);
							ccv_array_add_unique_int(block_dependencies[i].reads, id);
							if (!tensor_dependencies[id].reads)
								tensor_dependencies[id].reads = ccv_array_new(sizeof(int), 1, 0);
							ccv_array_add_unique_int(tensor_dependencies[id].reads, i);
						}
						_ccv_nnc_micro_block_dependencies_from_rvalue(&statements[k].compound_assignment.rvalue, i, block_dependencies, tensor_dependencies);
						break;
					}
				}
		}
	}
	*block_dependencies_ref = block_dependencies;
	*tensor_dependencies_ref = tensor_dependencies;
}

static void _ccv_nnc_micro_dependencies_free(ccv_nnc_micro_loop_block_dependency_t* const block_dependencies, const int block_size, ccv_nnc_micro_tensor_dependency_t* const tensor_dependencies, const int var_count)
{
	int i;
	for (i = 0; i < block_size; i++)
	{
		if (block_dependencies[i].writes)
			ccv_array_free(block_dependencies[i].writes);
		if (block_dependencies[i].reads)
			ccv_array_free(block_dependencies[i].reads);
	}
	ccfree(block_dependencies);
	for (i = 0; i < var_count; i++)
	{
		if (tensor_dependencies[i].writes)
			ccv_array_free(tensor_dependencies[i].writes);
		if (tensor_dependencies[i].reads)
			ccv_array_free(tensor_dependencies[i].reads);
	}
	ccfree(tensor_dependencies);
}

static int _ccv_nnc_tensor_reads_in_y_from_writes_after_x(const ccv_nnc_micro_loop_block_dependency_t* const block_dependencies, const ccv_nnc_micro_tensor_dependency_t* const tensor_dependencies, const int x, const int y)
{
	int i, j;
	int flag = 0;
	for (i = 0; !flag && i < block_dependencies[y].reads->rnum; i++)
	{
		const int read_idx = *(int*)ccv_array_get(block_dependencies[y].reads, i);
		if (tensor_dependencies[read_idx].writes)
			for (j = 0; !flag && j < tensor_dependencies[read_idx].writes->rnum; j++)
			{
				int block_idx = *(int*)ccv_array_get(tensor_dependencies[read_idx].writes, j);
				while (block_idx != block_dependencies[block_idx].merge_to)
					block_idx = block_dependencies[block_idx].merge_to;
				if (!block_dependencies[block_idx].flag) // Not in use, continue.
					continue;
				assert(block_idx <= y);
				// If the block_idx is between i and j (and not neither). We cannot merge.
				if (block_idx > x && block_idx != y)
					flag = block_idx;
			}
	}
	return flag;
}

static int _ccv_nnc_tensor_writes_in_x_reads_before_y(const ccv_nnc_micro_loop_block_dependency_t* const block_dependencies, const ccv_nnc_micro_tensor_dependency_t* const tensor_dependencies, const int x, const int y)
{
	int i, j;
	int flag = 0;
	for (i = 0; !flag && i < block_dependencies[x].writes->rnum; i++)
	{
		const int write_idx = *(int*)ccv_array_get(block_dependencies[x].writes, i);
		if (tensor_dependencies[write_idx].reads)
			for (j = 0; !flag && j < tensor_dependencies[write_idx].reads->rnum; j++)
			{
				int block_idx = *(int*)ccv_array_get(tensor_dependencies[write_idx].reads, j);
				while (block_idx != block_dependencies[block_idx].merge_to)
					block_idx = block_dependencies[block_idx].merge_to;
				if (!block_dependencies[block_idx].flag) // Not in use, continue.
					continue;
				assert(block_idx >= x);
				// If the block_idx is between i and j (and not neither). We cannot merge.
				if (block_idx < y && block_idx != x)
					flag = block_idx;
			}
	}
	return flag;
}

static void _ccv_nnc_tensor_remove_dead_store(const ccv_nnc_micro_tensor_dependency_t* const tensor_dependency, const int tensor_idx, ccv_array_t* const blocks)
{
	int i, j, k, l;;
	if (tensor_dependency->writes)
		for (i = 0; i < tensor_dependency->writes->rnum; i++)
		{
			const int write_idx = *(int*)ccv_array_get(tensor_dependency->writes, i);
			ccv_nnc_micro_loop_block_t* const block = (ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, write_idx);
			int flag = 0;
			for (j = 0; j < block->loop_count; j++)
			{
				ccv_nnc_micro_loop_statement_t* const statements = block->loops[j].statements;
				for (k = 0, l = 0; k < block->loops[j].statement_count; k++)
				{
					// It cannot be compound assignment, in this case, this tensor will be in read, and
					// it will be in active use (anything "read" in an active block will be marked as in use).
					assert(!(statements[k].type == CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_COMPOUND_ASSIGNMENT &&
						statements[k].compound_assignment.lvalue.id.type == CCV_NNC_MICRO_TENSOR_ID &&
						statements[k].compound_assignment.lvalue.id.id == tensor_idx));
					if (statements[k].type == CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_ASSIGNMENT &&
						statements[k].assignment.lvalue.id.type == CCV_NNC_MICRO_TENSOR_ID &&
						statements[k].assignment.lvalue.id.id == tensor_idx)
					{
						// This is a dead store, prepare to remove.
						ccv_nnc_micro_loop_statement_free(&statements[k]);
					} else {
						statements[l] = statements[k];
						++l;
					}
				}
				if (l < block->loops[j].statement_count)
				{
					if (l > 0)
						block->loops[j].statements = (ccv_nnc_micro_loop_statement_t*)ccrealloc(block->loops[j].statements, sizeof(ccv_nnc_micro_loop_statement_t) * l);
					else {
						ccfree(block->loops[j].statements);
						block->loops[j].statements = 0;
					}
					block->loops[j].statement_count = 0;
				}
				if (block->loops[j].statement_count > 0)
					flag = 1;
			}
			if (!flag) // No statement for this block, remove this whole block.
			{
				ccv_nnc_micro_loops_free(block->loops, block->loop_count);
				ccfree(block->loops);
				block->loops = 0;
				block->loop_count = 0;
			}
		}
}

static void _ccv_nnc_loop_merging(ccv_nnc_micro_loop_block_dependency_t* const block_dependencies, const ccv_nnc_micro_tensor_dependency_t* const tensor_dependencies, ccv_array_t* const blocks, const int max_loop_count, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	int i, j;
	int left_loop_idx[max_loop_count];
	int right_loop_idx[max_loop_count];
	ccv_nnc_micro_loop_t loops[max_loop_count];
	// Merge loops from blocks.
	for (i = 0; i < blocks->rnum - 1; i++)
	{
		ccv_nnc_micro_loop_block_t* const left_block = (ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, i);
		if (left_block->loop_count == 0)
			continue;
		for (j = i + 1; j < blocks->rnum; j++)
		{
			// We always merge from right block to left block. Thus, the right block will always be
			// in the original form.
			ccv_nnc_micro_loop_block_t* const right_block = (ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, j);
			if (right_block->loop_count == 0)
				continue;
			int merge_to_right = 0;
			// First check whether between left and right, there are any blocks that the right block
			// depends on. If that is the case, we cannot merge the right block into the left block.
			if (j > i + 1 && block_dependencies[j].reads)
			{
				const int block_idx = _ccv_nnc_tensor_reads_in_y_from_writes_after_x(block_dependencies, tensor_dependencies, i, j);
				// Cannot merge because we have dependencies in between. Merging will violate that
				// dependency relationship.
				if (block_idx)
				{
					// Now check to see if left can be merged into right? If so, we are lucky.
					if (_ccv_nnc_tensor_writes_in_x_reads_before_y(block_dependencies, tensor_dependencies, i, j))
						continue;
					merge_to_right = 1;
				}
			}
			// This method not only compares whether they have the same loop or not, but also gives indexes that
			// to match the loop start / end index, where they should move to. For example, if:
			// left_loop_idx[2] = 3
			// right_loop_idx[0] = 3
			// That means right now, loop at index 2 on the left is the same as loop at index 0 on the right.
			// And to match exactly, they both need to move to index 3.
			if (_ccv_nnc_same_loop(left_block, right_block, groups, axis_id_groups, left_loop_idx, right_loop_idx))
			{
				// Make sure if we have extra loop, they are on the left.
				if (right_block->loop_count > left_block->loop_count)
				{
					ccv_nnc_micro_loop_block_t t;
					CCV_SWAP(*left_block, *right_block, t);
				}
				assert(left_block->loop_count == right_block->loop_count || left_block->loop_count == right_block->loop_count + 1);
				_ccv_nnc_loop_order_by(left_block, left_loop_idx, loops);
				_ccv_nnc_loop_order_by(right_block, right_loop_idx, loops);
				const int left_start_idx = left_block->loop_count - right_block->loop_count;
				if (left_block->carried_count > 0)
					_ccv_nnc_loop_rename_carrieds(right_block, left_block->carried_count);
				left_block->carried_count += right_block->carried_count;
				int k;
				for (k = 0; k < right_block->loop_count; k++) // Merge loops.
				{
					const int left_idx = left_start_idx + k;
					if (right_block->loops[k].carried_count > 0)
					{
						if (left_block->loops[left_idx].carried_count > 0)
						{
							left_block->loops[left_idx].carrieds = (ccv_nnc_micro_loop_carried_t*)ccrealloc(left_block->loops[left_idx].carrieds, sizeof(ccv_nnc_micro_loop_carried_t) * (left_block->loops[left_idx].carried_count + right_block->loops[k].carried_count));
							memcpy(left_block->loops[left_idx].carrieds + left_block->loops[left_idx].carried_count, right_block->loops[k].carrieds, sizeof(ccv_nnc_micro_loop_carried_t) * right_block->loops[k].carried_count);
							ccfree(right_block->loops[k].carrieds);
						} else
							left_block->loops[left_idx].carrieds = right_block->loops[k].carrieds;
						left_block->loops[left_idx].carried_count += right_block->loops[k].carried_count;
						right_block->loops[k].carrieds = 0;
						right_block->loops[k].carried_count = 0;
					}
					if (right_block->loops[k].statement_count > 0)
					{
						if (left_block->loops[left_idx].statement_count > 0)
						{
							left_block->loops[left_idx].statements = (ccv_nnc_micro_loop_statement_t*)ccrealloc(left_block->loops[left_idx].statements, sizeof(ccv_nnc_micro_loop_statement_t) * (left_block->loops[left_idx].statement_count + right_block->loops[k].statement_count));
							memcpy(left_block->loops[left_idx].statements + left_block->loops[left_idx].statement_count, right_block->loops[k].statements, sizeof(ccv_nnc_micro_loop_statement_t) * right_block->loops[k].statement_count);
							ccfree(right_block->loops[k].statements);
						} else
							left_block->loops[left_idx].statements = right_block->loops[k].statements;
						left_block->loops[left_idx].statement_count += right_block->loops[k].statement_count;
						right_block->loops[k].statements = 0;
						right_block->loops[k].statement_count = 0;
					}
				}
				// Once merged, free the loop.
				ccfree(right_block->loops);
				right_block->loops = 0;
				right_block->loop_count = 0;
				int x = i, y = j;
				if (merge_to_right) // If this is merge to right.
				{
					ccv_nnc_micro_loop_block_t t;
					CCV_SWAP(*left_block, *right_block, t);
					x = j, y = i;
				}
				// Merge all reads and writes tensors into block dependency.
				if (block_dependencies[y].writes && block_dependencies[y].writes->rnum)
				{
					if (!block_dependencies[x].writes)
						block_dependencies[x].writes = ccv_array_new(sizeof(int), 1, 0);
					for (k = 0; k < block_dependencies[y].writes->rnum; k++)
						ccv_array_push(block_dependencies[x].writes, ccv_array_get(block_dependencies[y].writes, k));
				}
				if (block_dependencies[y].reads && block_dependencies[y].reads->rnum)
				{
					if (!block_dependencies[x].reads)
						block_dependencies[x].reads = ccv_array_new(sizeof(int), 1, 0);
					for (k = 0; k < block_dependencies[y].reads->rnum; k++)
						ccv_array_push(block_dependencies[x].reads, ccv_array_get(block_dependencies[y].reads, k));
				}
				// Merged, mark the proper merging dependency.
				block_dependencies[y].merge_to = x;
				if (merge_to_right) // If this is merge to right, now left is empty, break.
					break;
			}
		}
	}
}

static void _ccv_nnc_var_subst(ccv_nnc_micro_tensor_t* const vars, const int var_count, const ccv_nnc_micro_io_t* const inputs, const int input_size, const ccv_nnc_micro_io_t* const outputs, const int output_size, ccv_array_t* const blocks, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	int i, j;
	// These are simple programs, so we are going to loop over all blocks to see whether a non-output-input
	// var only write / read in one loop. If that is the case, we are going to remove that var.
	// We have to do this replacement from bottom to top though.
	for (i = 0; i < var_count; i++)
	{
		int flag = 0;
		for (j = 0; !flag && j < input_size; j++)
			flag = (inputs[j]->id == i);
		for (j = 0; !flag && j < output_size; j++)
			flag = (outputs[j]->id == i);
		if (flag) // This is in outputs or inputs.
			continue;
		int count_var = 0;
		ccv_nnc_micro_loop_variable_t lvalue;
		ccv_nnc_micro_loop_expression_t rvalue;
		int block_idx, loop_idx, statement_idx;
		for (j = 0; j < blocks->rnum; j++)
		{
			const ccv_nnc_micro_loop_block_t* const block = (ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, j);
			int k, l;
			const int loop_count = block->loop_count;
			const ccv_nnc_micro_loop_t* const loops = block->loops;
			int var_per_block = 0;
			for (k = 0; k < loop_count; k++)
			{
				int flag = 0;
				const int statement_count = loops[k].statement_count;
				ccv_nnc_micro_loop_statement_t* const statements = loops[k].statements;
				for (l = 0; l < statement_count; l++)
					if (statements[l].type == CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_ASSIGNMENT &&
						statements[l].assignment.lvalue.id.type == CCV_NNC_MICRO_TENSOR_ID &&
						statements[l].assignment.lvalue.id.id == i)
					{
						lvalue = statements[l].assignment.lvalue;
						if (_ccv_nnc_only_var_in_rvalue(i, lvalue, statements[l], groups, axis_id_groups))
							flag = 2;
						else {
							// If the variable not showing up on the right-side, we can continue.
							rvalue = statements[l].assignment.rvalue;
							block_idx = j;
							loop_idx = k;
							statement_idx = l;
							++flag;
						}
					} else if (statements[l].type == CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_COMPOUND_ASSIGNMENT &&
						statements[l].compound_assignment.lvalue.id.type == CCV_NNC_MICRO_TENSOR_ID &&
						statements[l].compound_assignment.lvalue.id.id == i) {
						// This is compound assignment, automatically increase by 2.
						flag += 2;
					}
				if (flag > 1) // We have more than 1 assignment for this id, it is not good. We cannot remove it.
				{
					var_per_block += flag;
					continue;
				}
				for (l = 0; l < statement_count; l++)
					flag = ccv_max(flag, _ccv_nnc_only_var_in_rvalue(i, lvalue, statements[l], groups, axis_id_groups));
				// If flag == 2, meaning it found a var with a different index. This is a bad news.
				var_per_block += flag;
			}
			count_var += var_per_block;
		}
		// If this is used more than one place (write multiple times, have different index, or used in different blocks),
		// I cannot get rid of it.
		if (count_var != 1)
			continue;
		// Otherwise, now loop again and prepare to get rid of it.
		ccv_nnc_micro_loop_block_t* const block = (ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, block_idx);
		ccv_nnc_micro_loop_statement_t* statements = block->loops[loop_idx].statements;
		ccv_nnc_micro_loop_statement_t statement = statements[statement_idx];
		// First, remove the assignment.
		if (statement_idx < block->loops[loop_idx].statement_count - 1)
			memmove(statements + statement_idx, statements + statement_idx + 1, sizeof(ccv_nnc_micro_loop_statement_t) * (block->loops[loop_idx].statement_count - statement_idx - 1));
		--block->loops[loop_idx].statement_count;
		const int statement_count = block->loops[loop_idx].statement_count;
		statements = block->loops[loop_idx].statements = (ccv_nnc_micro_loop_statement_t*)ccrealloc(statements, sizeof(ccv_nnc_micro_loop_statement_t) * statement_count);
		int k = 0;
		for (j = 0; j < statement_count; j++)
			_ccv_nnc_replacing_id_in_rvalue(&statements[j], i, rvalue, &k);
		if (k == 0) // If nothing to replace, free up everything.
			ccv_nnc_micro_loop_statement_free(&statement);
		else
			ccv_nnc_micro_loop_statement_lvalue_free(&statement);
		// No need to allocate for this var. It is not used, only useful for shape computation.
		vars[i].no_alloc = 1;
	}
}

static int _ccv_nnc_index_binary_size(const ccv_nnc_micro_loop_index_term_t index)
{
	switch (index.type)
	{
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_NONE:
			return 0;
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL:
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_ID:
			return 1;
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY:
			if (index.binary->op == CCV_NNC_MICRO_BINARY_OP_PLUS || index.binary->op == CCV_NNC_MICRO_BINARY_OP_MINUS)
				return _ccv_nnc_index_binary_size(index.binary->left) + _ccv_nnc_index_binary_size(index.binary->right);
			else
				return 1;
	}
	return 0;
}

typedef struct {
	int sign:7;
	int ignore:1;
	ccv_nnc_micro_loop_index_term_t term;
} ccv_nnc_micro_loop_binary_term_t;

static void _ccv_nnc_index_term_flatten(ccv_nnc_micro_loop_binary_term_t* const binary_terms, const ccv_nnc_micro_loop_index_term_t index, const int sign, int* const i)
{
	switch (index.type)
	{
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_NONE: // No need to occupy.
			break;
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL:
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_ID:
			binary_terms[*i].term = index;
			binary_terms[*i].sign = sign;
			binary_terms[*i].ignore = 0;
			++(*i);
			break;
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY:
			if (index.binary->op == CCV_NNC_MICRO_BINARY_OP_PLUS || index.binary->op == CCV_NNC_MICRO_BINARY_OP_MINUS)
			{
				_ccv_nnc_index_term_flatten(binary_terms, index.binary->left, sign, i);
				if (index.binary->op == CCV_NNC_MICRO_BINARY_OP_MINUS) // Switch sign.
					_ccv_nnc_index_term_flatten(binary_terms, index.binary->right, sign == CCV_NNC_MICRO_BINARY_OP_PLUS ? CCV_NNC_MICRO_BINARY_OP_MINUS : CCV_NNC_MICRO_BINARY_OP_PLUS, i);
				else
					_ccv_nnc_index_term_flatten(binary_terms, index.binary->right, sign, i);
			} else {
				binary_terms[*i].term = index;
				binary_terms[*i].sign = sign;
				binary_terms[*i].ignore = 0;
				++(*i);
			}
			break;
	}
}

// 0 is we don't understand, -1 is false, 1 is true.
static int _ccv_nnc_index_less_than_or_equal_to(const ccv_nnc_micro_loop_index_term_t left, const ccv_nnc_micro_loop_index_term_t right, const ccv_nnc_micro_tensor_t* const vars, const int var_count, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	// Special case 1.
	if (left.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL && right.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL)
		return left.immediate_value <= right.immediate_value ? 1 : -1;
	// Special case 2.
	if (left.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL && left.immediate_value == 0 && right.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_ID && right.id.type == CCV_NNC_MICRO_AXIS_SIZE_ID)
		return 1;
	// Special case 3.
	if (left.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_ID && left.id.type == CCV_NNC_MICRO_AXIS_SIZE_ID && right.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL && right.immediate_value == 0)
		return -1;
	// Now, we only have one variable in both left and right, need to flat the binary tree (if possible) and reduce it to constant if possible.
	// We can only flatten if it is + / - at the moment.
	const int left_binary_size = _ccv_nnc_index_binary_size(left);
	assert(left_binary_size >= 1);
	const int right_binary_size = _ccv_nnc_index_binary_size(right);
	assert(right_binary_size >= 1);
	ccv_nnc_micro_loop_binary_term_t* const left_binary_terms = (ccv_nnc_micro_loop_binary_term_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_binary_term_t) * (left_binary_size + right_binary_size));
	ccv_nnc_micro_loop_binary_term_t* const right_binary_terms = left_binary_terms + left_binary_size;
	int i, j;
	i = 0;
	_ccv_nnc_index_term_flatten(left_binary_terms, left, CCV_NNC_MICRO_BINARY_OP_PLUS, &i);
	assert(i == left_binary_size);
	i = 0;
	_ccv_nnc_index_term_flatten(right_binary_terms, right, CCV_NNC_MICRO_BINARY_OP_PLUS, &i);
	assert(i == right_binary_size);
	// Matching signs in left terms.
	for (i = 0; i < left_binary_size - 1; i++)
		for (j = i + 1; j < left_binary_size; j++)
			if (!left_binary_terms[i].ignore && !left_binary_terms[j].ignore &&
				_ccv_nnc_same_index_term(left_binary_terms[i].term, left_binary_terms[j].term, groups, axis_id_groups) &&
				left_binary_terms[i].sign != left_binary_terms[j].sign)
			{
				left_binary_terms[i].ignore = -1;
				left_binary_terms[j].ignore = -1;
			}
	// Matching signs in right terms.
	for (i = 0; i < right_binary_size - 1; i++)
		for (j = i + 1; j < right_binary_size; j++)
			if (!right_binary_terms[i].ignore && !right_binary_terms[j].ignore &&
				_ccv_nnc_same_index_term(right_binary_terms[i].term, right_binary_terms[j].term, groups, axis_id_groups) &&
				right_binary_terms[i].sign != right_binary_terms[j].sign)
			{
				right_binary_terms[i].ignore = -1;
				right_binary_terms[j].ignore = -1;
			}
	// Matching left to right.
	for (i = 0; i < left_binary_size; i++)
		for (j = 0; j < right_binary_size; j++)
			// If they are the same, we can ignore now.
			if (!left_binary_terms[i].ignore && !right_binary_terms[j].ignore &&
				_ccv_nnc_same_index_term(left_binary_terms[i].term, right_binary_terms[j].term, groups, axis_id_groups) &&
				left_binary_terms[i].sign == right_binary_terms[j].sign)
			{
				left_binary_terms[i].ignore = -1;
				right_binary_terms[j].ignore = -1;
			}
	// After reduced, we should only have immediate values left, otherwise we cannot progress.
	int left_val = 0;
	for (i = 0; i < left_binary_size; i++)
		if (!left_binary_terms[i].ignore)
		{
			if (left_binary_terms[i].term.type != CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL)
			{
				ccfree(left_binary_terms);
				return 0;
			} else
				left_val += left_binary_terms[i].sign == CCV_NNC_MICRO_BINARY_OP_PLUS ? left_binary_terms[i].term.immediate_value : -left_binary_terms[i].term.immediate_value;
		}
	int right_val = 0;
	for (i = 0; i < right_binary_size; i++)
		if (!right_binary_terms[i].ignore)
		{
			if (right_binary_terms[i].term.type != CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL)
			{
				ccfree(left_binary_terms);
				return 0;
			} else
				right_val += right_binary_terms[i].sign == CCV_NNC_MICRO_BINARY_OP_PLUS ? right_binary_terms[i].term.immediate_value : -right_binary_terms[i].term.immediate_value;
		}
	ccfree(left_binary_terms);
	return left_val <= right_val ? 1 : -1;
}

// If this index term refers to an axis size that actually has a expression, refer to that instead (like for reindex operation).
static ccv_nnc_micro_loop_index_term_t _ccv_nnc_micro_index_shape_merging(const ccv_nnc_micro_loop_index_term_t index, const ccv_nnc_micro_tensor_t* const vars, const int var_count, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	ccv_nnc_micro_loop_index_term_t result = index;
	for (;;)
	{
		if (!(result.type == CCV_NNC_MICRO_LOOP_INDEX_TYPE_ID && result.id.type == CCV_NNC_MICRO_AXIS_SIZE_ID))
			return result;
		int root = groups[result.id.id];
		while (groups[root] != root)
			root = groups[root];
		if (vars[root].shape == 0)
			return result;
		assert(result.id.d >= 0 && result.id.d < vars[root].dimensions);
		result = vars[root].shape[result.id.d];
	}
}

static int _ccv_nnc_micro_low_high_bound_from_index(const ccv_nnc_micro_loop_index_term_t index, ccv_nnc_micro_loop_index_term_t* const low_ref, ccv_nnc_micro_loop_index_term_t* const high_ref, const ccv_nnc_micro_loop_t* const loops, const int loop_count, const ccv_nnc_micro_tensor_t* const vars, const int var_count, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	switch (index.type)
	{
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_NONE:
			*low_ref = (ccv_nnc_micro_loop_index_term_t){
				.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL,
				.immediate_value = 0
			};
			*high_ref = (ccv_nnc_micro_loop_index_term_t){
				.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL,
				.immediate_value = 0
			};
			return 1;
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_ID:
			if (index.id.type == CCV_NNC_MICRO_LOOP_ID)
			{
				int loop_idx = -1;
				int i;
				for (i = 0; loop_idx < 0 && i < loop_count; i++)
					if (loops[i].id.id == index.id.id)
						loop_idx = i;
				assert(loop_idx >= 0);
				const ccv_nnc_micro_loop_index_term_t start_index = _ccv_nnc_micro_index_shape_merging(loops[loop_idx].start_index, vars, var_count, groups, axis_id_groups);
				const ccv_nnc_micro_loop_index_term_t end_index = _ccv_nnc_micro_index_shape_merging(loops[loop_idx].end_index, vars, var_count, groups, axis_id_groups);
				*low_ref = ccv_nnc_micro_loop_index_deep_copy(&start_index);
				*high_ref = ccv_nnc_micro_loop_index_deep_copy(&end_index);
			} else {
				*low_ref = index;
				*high_ref = index;
			}
			return 1;
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL:
			*low_ref = index;
			*high_ref = index;
			return 1;
		case CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY: {
			// Get low, high from both left and right, and then construct new low / high.
			ccv_nnc_micro_loop_index_term_t left_low, left_high;
			if (!_ccv_nnc_micro_low_high_bound_from_index(index.binary->left, &left_low, &left_high, loops, loop_count, vars, var_count, groups, axis_id_groups))
				return 0;
			ccv_nnc_micro_loop_index_term_t right_low, right_high;
			if (!_ccv_nnc_micro_low_high_bound_from_index(index.binary->right, &right_low, &right_high, loops, loop_count, vars, var_count, groups, axis_id_groups))
			{
				ccv_nnc_micro_loop_index_free(&left_low);
				ccv_nnc_micro_loop_index_free(&left_high);
				return 0;
			}
			// If left is not a range, or right is not a range, it is simple, just copy over.
			if (_ccv_nnc_same_index_term(left_low, left_high, groups, axis_id_groups) || _ccv_nnc_same_index_term(right_low, right_high, groups, axis_id_groups))
			{
				*low_ref = (ccv_nnc_micro_loop_index_term_t){
					.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY,
					.binary = (ccv_nnc_micro_loop_index_binary_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_index_binary_t))
				};
				low_ref->binary->op = index.binary->op;
				low_ref->binary->left = left_low;
				low_ref->binary->right = right_low;
				*high_ref = (ccv_nnc_micro_loop_index_term_t){
					.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY,
					.binary = (ccv_nnc_micro_loop_index_binary_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_index_binary_t))
				};
				high_ref->binary->op = index.binary->op;
				high_ref->binary->left = left_high;
				high_ref->binary->right = right_high;
				return 1;
			}
			// Cannot handle -, because lower bound will go to negative, similar for /. Only can handle + and *.
			if (!(index.binary->op == CCV_NNC_MICRO_BINARY_OP_PLUS || index.binary->op == CCV_NNC_MICRO_BINARY_OP_MUL) ||
				// If lower bound is not a non-negative integer, we cannot compute interesting low / high bound, abort.
				(left_low.type != CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL || left_low.immediate_value < 0) ||
				(right_low.type != CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL || right_low.immediate_value < 0))
			{
				ccv_nnc_micro_loop_index_free(&left_low);
				ccv_nnc_micro_loop_index_free(&left_high);
				ccv_nnc_micro_loop_index_free(&right_low);
				ccv_nnc_micro_loop_index_free(&right_high);
				return 0;
			}
			*low_ref = (ccv_nnc_micro_loop_index_term_t){
				.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL,
				.immediate_value = index.binary->op == CCV_NNC_MICRO_BINARY_OP_PLUS ? left_low.immediate_value + right_low.immediate_value : left_low.immediate_value * right_low.immediate_value,
			};
			// higher bound is not inclusive, hence, we need to minus extra 1 for this.
			if (index.binary->op == CCV_NNC_MICRO_BINARY_OP_PLUS)
			{
				// (left - 1) + (right - 1) + 1
				ccv_nnc_micro_loop_index_term_t sum = {
					.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY,
					.binary = (ccv_nnc_micro_loop_index_binary_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_index_binary_t))
				};
				sum.binary->op = CCV_NNC_MICRO_BINARY_OP_PLUS;
				sum.binary->left = left_high;
				sum.binary->right = right_high;
				*high_ref = (ccv_nnc_micro_loop_index_term_t){
					.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY,
					.binary = (ccv_nnc_micro_loop_index_binary_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_index_binary_t))
				};
				high_ref->binary->op = CCV_NNC_MICRO_BINARY_OP_MINUS;
				high_ref->binary->left = sum;
				high_ref->binary->right = (ccv_nnc_micro_loop_index_term_t){
					.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL,
					.immediate_value = 1
				};
			} else {
				// (left - 1) * (right - 1) + 1
				ccv_nnc_micro_loop_index_term_t prod = {
					.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY,
					.binary = (ccv_nnc_micro_loop_index_binary_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_index_binary_t))
				};
				prod.binary->op = CCV_NNC_MICRO_BINARY_OP_MUL;
				prod.binary->left = left_high;
				prod.binary->right = right_high;
				ccv_nnc_micro_loop_index_term_t minus_left = {
					.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY,
					.binary = (ccv_nnc_micro_loop_index_binary_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_index_binary_t))
				};
				minus_left.binary->op = CCV_NNC_MICRO_BINARY_OP_MINUS;
				minus_left.binary->left = prod;
				minus_left.binary->right = left_high;
				ccv_nnc_micro_loop_index_term_t minus_right = {
					.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY,
					.binary = (ccv_nnc_micro_loop_index_binary_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_index_binary_t))
				};
				minus_right.binary->op = CCV_NNC_MICRO_BINARY_OP_MINUS;
				minus_right.binary->left = minus_left;
				minus_right.binary->right = right_high;
				*high_ref = (ccv_nnc_micro_loop_index_term_t){
					.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY,
					.binary = (ccv_nnc_micro_loop_index_binary_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_index_binary_t))
				};
				high_ref->binary->op = CCV_NNC_MICRO_BINARY_OP_PLUS;
				high_ref->binary->left = minus_right;
				high_ref->binary->right = (ccv_nnc_micro_loop_index_term_t){
					.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL,
					.immediate_value = 2
				};
			}
			return 1;
		}
	}
	return 0;
}

static void _ccv_nnc_micro_check_bound_for_variable(ccv_nnc_micro_loop_variable_t* const variable, const ccv_nnc_micro_loop_t* const loops, const int loop_count, const ccv_nnc_micro_tensor_t* const vars, const int var_count, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	if (variable->id.type != CCV_NNC_MICRO_TENSOR_ID)
		return;
	int i, j;
	assert(variable->id.id >= 0 && variable->id.id < var_count);
	ccv_nnc_micro_loop_index_term_t index_zero = {
		.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL,
		.immediate_value = 0
	};
	for (i = 0; i < variable->index_count; i++)
	{
		const ccv_nnc_micro_loop_index_term_t shape = _ccv_nnc_micro_index_shape_merging((ccv_nnc_micro_loop_index_term_t){
			.type = CCV_NNC_MICRO_LOOP_INDEX_TYPE_ID,
			.id = {
				.type = CCV_NNC_MICRO_AXIS_SIZE_ID,
				.id = variable->id.id,
				.d = i
			}
		}, vars, var_count, groups, axis_id_groups);
		switch (variable->index[i].type)
		{
			case CCV_NNC_MICRO_LOOP_INDEX_TYPE_ID:
				// For loop id, we can check the range to see if it is within the shape.
				if (variable->index[i].id.type == CCV_NNC_MICRO_LOOP_ID)
				{
					int loop_idx = -1;
					for (j = 0; loop_idx < 0 && j < loop_count; j++)
						if (loops[j].id.id == variable->index[i].id.id)
							loop_idx = j;
					assert(loop_idx >= 0);
					const ccv_nnc_micro_loop_index_term_t start_index = _ccv_nnc_micro_index_shape_merging(loops[loop_idx].start_index, vars, var_count, groups, axis_id_groups);
					const ccv_nnc_micro_loop_index_term_t end_index = _ccv_nnc_micro_index_shape_merging(loops[loop_idx].end_index, vars, var_count, groups, axis_id_groups);
					if (_ccv_nnc_index_less_than_or_equal_to(index_zero, start_index, vars, var_count, groups, axis_id_groups) == 1 &&
						_ccv_nnc_index_less_than_or_equal_to(end_index, shape, vars, var_count, groups, axis_id_groups) == 1)
						variable->no_check_bound[i] = 1;
					else
						variable->no_check_bound[i] = 0;
				} else // If it is anything other than loop id, we have to check the bound.
					variable->no_check_bound[i] = 0;
				break;
			case CCV_NNC_MICRO_LOOP_INDEX_TYPE_BINARY: {
				// Compute higher / lower bounds along the expression.
				ccv_nnc_micro_loop_index_term_t low, high;
				// Cannot find high low, mark no_check_bound[i] = 0
				if (!_ccv_nnc_micro_low_high_bound_from_index(variable->index[i], &low, &high, loops, loop_count, vars, var_count, groups, axis_id_groups))
				{
					variable->no_check_bound[i] = 0;
					break;
				}
				if (_ccv_nnc_index_less_than_or_equal_to(index_zero, low, vars, var_count, groups, axis_id_groups) == 1 &&
					_ccv_nnc_index_less_than_or_equal_to(high, shape, vars, var_count, groups, axis_id_groups) == 1)
					variable->no_check_bound[i] = 1;
				else
					variable->no_check_bound[i] = 0;
				ccv_nnc_micro_loop_index_free(&low);
				ccv_nnc_micro_loop_index_free(&high);
				break;
			}
			case CCV_NNC_MICRO_LOOP_INDEX_TYPE_VAL:
				// If the index is an integer, and it is bigger than 0, we need to check bound (there is no assertion the end index is larger than anything other than 0).
				if (variable->index[i].immediate_value == 0)
					variable->no_check_bound[i] = 1;
				else
					variable->no_check_bound[i] = 0;
				break;
		}
	}
}

static void _ccv_nnc_micro_check_bound_for_expression(ccv_nnc_micro_loop_expression_t* const expression, const ccv_nnc_micro_loop_t* const loops, const int loop_count, const ccv_nnc_micro_tensor_t* const vars, const int var_count, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	switch (expression->type)
	{
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_VAR:
			_ccv_nnc_micro_check_bound_for_variable(&expression->variable, loops, loop_count, vars, var_count, groups, axis_id_groups);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_TERNAY:
			_ccv_nnc_micro_check_bound_for_expression(expression->ternary.pivot, loops, loop_count, vars, var_count, groups, axis_id_groups);
			_ccv_nnc_micro_check_bound_for_expression(expression->ternary.left, loops, loop_count, vars, var_count, groups, axis_id_groups);
			_ccv_nnc_micro_check_bound_for_expression(expression->ternary.right, loops, loop_count, vars, var_count, groups, axis_id_groups);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_BINARY:
			_ccv_nnc_micro_check_bound_for_expression(expression->binary.left, loops, loop_count, vars, var_count, groups, axis_id_groups);
			_ccv_nnc_micro_check_bound_for_expression(expression->binary.right, loops, loop_count, vars, var_count, groups, axis_id_groups);
			break;
		case CCV_NNC_MICRO_LOOP_EXPR_TYPE_UNARY:
			_ccv_nnc_micro_check_bound_for_expression(expression->unary.x, loops, loop_count, vars, var_count, groups, axis_id_groups);
			break;
	}
}

static void _ccv_nnc_micro_check_bound_for_block(ccv_nnc_micro_loop_block_t* const block, const ccv_nnc_micro_tensor_t* const vars, const int var_count, const int* const groups, khash_t(ccv_nnc_axis_id_group)* const axis_id_groups)
{
	int i, j;
	for (i = 0; i < block->loop_count; i++)
	{
		const int statement_count = block->loops[i].statement_count;
		ccv_nnc_micro_loop_statement_t* const statements = block->loops[i].statements;
		for (j = 0; j < statement_count; j++)
		{
			switch (statements[j].type)
			{
				case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_ASSIGNMENT:
					_ccv_nnc_micro_check_bound_for_variable(&statements[j].assignment.lvalue, block->loops, block->loop_count, vars, var_count, groups, axis_id_groups);
					_ccv_nnc_micro_check_bound_for_expression(&statements[j].assignment.rvalue, block->loops, block->loop_count, vars, var_count, groups, axis_id_groups);
					break;
				case CCV_NNC_MICRO_LOOP_STATEMENT_TYPE_COMPOUND_ASSIGNMENT:
					if (statements[j].compound_assignment.lvalue.type == CCV_NNC_MICRO_LOOP_EXPR_TYPE_VAR)
						_ccv_nnc_micro_check_bound_for_variable(&statements[j].compound_assignment.lvalue.variable, block->loops, block->loop_count, vars, var_count, groups, axis_id_groups);
					_ccv_nnc_micro_check_bound_for_expression(&statements[j].compound_assignment.rvalue, block->loops, block->loop_count, vars, var_count, groups, axis_id_groups);
					break;
			}
		}
	}
}

void ccv_nnc_micro_program_simplify(ccv_nnc_micro_program_t* const program, const ccv_nnc_micro_io_t* const inputs, const int input_size, const ccv_nnc_micro_io_t* const outputs, const int output_size, const ccv_array_t* const equal_assertions)
{
	// Nothing to simplify for.
	if (program->function_count < 1)
		return;
	// Only one block, nothing to simplify for.
	if (program->function_count == 1 && program->functions[0].block_count == 1)
		return;
	if (input_size == 0 || output_size == 0)
		return;
	// Union-find to group all variables with the same shape.
	ccv_nnc_micro_tensor_t* const vars = program->vars;
	const int var_count = program->var_count;
	int* const groups = (int*)ccmalloc(sizeof(int) * var_count);
	int i, j;
	for (i = 0; i < var_count; i++)
		groups[i] = i;
	// If no shape, they should match these input.
	for (i = 0; i < var_count; i++)
		if (vars[i].input >= 0 && !vars[i].shape)
		{
			int root = vars[i].input;
			while (groups[root] != root)
				root = groups[root];
			groups[i] = root;
		}
	for (i = 0; i < var_count; i++)
	{
		// If this is input (no other tensor as the input), we skip.
		if (vars[i].input < 0)
			continue;
		int root = i;
		while (groups[root] != root)
			root = groups[root];
		// If the sibling exists and we haven't visited yet, mark them has the same group as us.
		if (vars[i].sibling >= 0 && vars[i].sibling < i && groups[vars[i].sibling] < 0)
			groups[vars[i].sibling] = root;
	}
	for (i = var_count - 1; i > 0; i--)
	{
		// Now matching the shape.
		if (vars[i].input < 0 || !vars[i].shape)
			continue;
		int root = i;
		while (groups[root] != root)
			root = groups[root];
		for (j = i - 1; j >= 0; j--)
			if (vars[j].shape && vars[j].dimensions == vars[i].dimensions &&
				_ccv_nnc_same_shape(vars[j].shape, vars[i].shape, vars[i].dimensions))
				groups[j] = root;
	}
	// Group equal assertions on axis together.
	khash_t(ccv_nnc_axis_id_group)* const axis_id_groups = kh_init(ccv_nnc_axis_id_group);
	for (i = 0; i < equal_assertions->rnum; i++)
	{
		const ccv_nnc_micro_id_equal_assertion_t* const equal_assertion = (ccv_nnc_micro_id_equal_assertion_t*)ccv_array_get(equal_assertions, i);
		ccv_nnc_micro_id_t left = equal_assertion->left;
		while (groups[left.id] != left.id)
			left.id = groups[left.id];
		int left_root = MICRO_ID_TO_INT(left);
		khiter_t k;
		for (;;) {
			k = kh_get(ccv_nnc_axis_id_group, axis_id_groups, left_root);
			if (k == kh_end(axis_id_groups))
				break;
			left_root = kh_val(axis_id_groups, k);
		}
		ccv_nnc_micro_id_t right = equal_assertion->right;
		while (groups[right.id] != right.id)
			left.id = groups[right.id];
		int right_root = MICRO_ID_TO_INT(equal_assertion->right);
		for (;;) {
			k = kh_get(ccv_nnc_axis_id_group, axis_id_groups, right_root);
			if (k == kh_end(axis_id_groups))
				break;
			right_root = kh_val(axis_id_groups, k);
		}
		if (left_root != right_root) // k is the right root at the moment.
		{
			int ret;
			k = kh_put(ccv_nnc_axis_id_group, axis_id_groups, right_root, &ret);
			assert(ret != 0);
			kh_val(axis_id_groups, k) = left_root;
		}
	}
	// First, flat out all functions into blocks.
	ccv_array_t* const blocks = ccv_array_new(sizeof(ccv_nnc_micro_loop_block_t), 0, 0);
	ccv_nnc_micro_function_t* const functions = program->functions;
	const int function_count = program->function_count;
	int max_loop_count = 0;
	for (i = 0; i < function_count; i++)
	{
		const int block_count = functions[i].block_count;
		ccv_nnc_micro_loop_block_t* const function_blocks = block_count == 1 ? &functions[i].one_block : functions[i].blocks;
		for (j = 0; j < block_count; j++)
		{
			max_loop_count = ccv_max(function_blocks[j].loop_count, max_loop_count);
			ccv_array_push(blocks, &function_blocks[j]);
		}
	}
	// Next, find dependencies between these function blocks and marking these that are dependencies for the final outputs.
	// We need to build our connections between blocks <-> r/w vars.
	ccv_nnc_micro_loop_block_dependency_t* block_dependencies;
	ccv_nnc_micro_tensor_dependency_t* tensor_dependencies;
	const int block_size = blocks->rnum;
	_ccv_nnc_micro_block_dependencies((ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, 0), block_size, var_count, &block_dependencies, &tensor_dependencies);
	ccv_array_t* const in_use = ccv_array_new(sizeof(int), output_size, 0);
	// Use the dependencies to mark blocks / vars that are in use.
	for (i = 0; i < output_size; i++)
	{
		tensor_dependencies[outputs[i]->id].flag = 1; // Mark them as in use.
		ccv_array_push(in_use, &outputs[i]->id);
	}
	for (i = 0; i < input_size; i++)
		tensor_dependencies[inputs[i]->id].flag = 1; // Mark inputs as in use so we don't go pass them.
	for (i = 0; i < in_use->rnum; i++)
	{
		const int tensor_idx = *(int*)ccv_array_get(in_use, i);
		if (tensor_dependencies[tensor_idx].writes)
			for (j = 0; j < tensor_dependencies[tensor_idx].writes->rnum; j++)
			{
				const int block_idx = *(int*)ccv_array_get(tensor_dependencies[tensor_idx].writes, j);
				block_dependencies[block_idx].flag = 1;
				int k;
				if (block_dependencies[block_idx].reads)
					for (k = 0; k < block_dependencies[block_idx].reads->rnum; k++)
					{
						const int read_idx = *(int*)ccv_array_get(block_dependencies[block_idx].reads, k);
						if (!tensor_dependencies[read_idx].flag)
						{
							tensor_dependencies[read_idx].flag = 1;
							ccv_array_push(in_use, &read_idx);
						}
					}
			}
	}
	ccv_array_free(in_use);
	for (i = 0; i < block_size; i++)
		if (!block_dependencies[i].flag)
		{
			ccv_nnc_micro_loop_block_t* const block = (ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, i);
			ccv_nnc_micro_loops_free(block->loops, block->loop_count);
			ccfree(block->loops);
			block->loops = 0;
			block->loop_count = 0;
		}
	for (i = 0; i < var_count; i++)
		if (!tensor_dependencies[i].flag) // If this tensor is not visited, there is no need to alloc.
		{
			_ccv_nnc_tensor_remove_dead_store(&tensor_dependencies[i], i, blocks);
			vars[i].no_alloc = 1;
		}
	_ccv_nnc_loop_merging(block_dependencies, tensor_dependencies, blocks, max_loop_count, groups, axis_id_groups);
	_ccv_nnc_micro_dependencies_free(block_dependencies, block_size, tensor_dependencies, var_count);
	// Culling out empty blocks.
	for (i = 0, j = 0; i < blocks->rnum; i++)
	{
		const ccv_nnc_micro_loop_block_t* const block = (ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, i);
		if (block->loop_count > 0)
		{
			*(ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, j) = *block;
			++j;
		}
	}
	// Now we moved everything, set the proper block size.
	ccv_array_resize(blocks, j);
	// Substitute variables.
	_ccv_nnc_var_subst(vars, var_count, inputs, input_size, outputs, output_size, blocks, groups, axis_id_groups);
	// Mark whether we need to check bound for a particular variable or not.
	for (i = 0; i < blocks->rnum; i++)
	{
		ccv_nnc_micro_loop_block_t* const block = (ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, i);
		_ccv_nnc_micro_check_bound_for_block(block, vars, var_count, groups, axis_id_groups);
	}
	free(groups);
	kh_destroy(ccv_nnc_axis_id_group, axis_id_groups);
	// Reallocate function to be 1.
	for (i = 0; i < function_count; i++)
		if (functions[i].block_count > 1)
			ccfree(functions[i].blocks);
	program->functions = (ccv_nnc_micro_function_t*)ccrealloc(program->functions, sizeof(ccv_nnc_micro_function_t));
	program->functions[0].block_count = blocks->rnum;
	if (blocks->rnum > 1)
	{
		program->functions[0].blocks = (ccv_nnc_micro_loop_block_t*)ccmalloc(sizeof(ccv_nnc_micro_loop_block_t) * blocks->rnum);
		memcpy(program->functions[0].blocks, ccv_array_get(blocks, 0), sizeof(ccv_nnc_micro_loop_block_t) * blocks->rnum);
	} else
		program->functions[0].one_block = *(ccv_nnc_micro_loop_block_t*)ccv_array_get(blocks, 0);
	program->function_count = 1;
	ccv_array_free(blocks);
}
