/*
 * Copyright (c) 2017 Cesanta Software Limited
 * All rights reserved
 */

#include "mjs/src/mjs_array.h"
#include "mjs/src/mjs_bcode.h"
#include "mjs/src/mjs_conversion.h"
#include "mjs/src/mjs_core.h"
#include "mjs/src/mjs_exec.h"
#include "mjs/src/mjs_internal.h"
#include "mjs/src/mjs_object.h"
#include "mjs/src/mjs_parser.h"
#include "mjs/src/mjs_primitive.h"
#include "mjs/src/mjs_string.h"
#include "mjs/src/mjs_tok.h"
#include "mjs/src/mjs_util.h"
#include "mjs/src/mjs_varint.h"

static mjs_val_t mjs_find_scope(struct mjs *mjs, mjs_val_t key) {
  size_t num_scopes = mjs_stack_size(&mjs->scopes);
  while (num_scopes > 0) {
    mjs_val_t scope = *vptr(&mjs->scopes, num_scopes - 1);
    num_scopes--;
    if (mjs_get_own_property_v(mjs, scope, key) != NULL) return scope;
  }
  mjs_set_errorf(mjs, MJS_REFERENCE_ERROR, "[%s] is not defined",
                 mjs_get_cstring(mjs, &key));
  return MJS_UNDEFINED;
}

mjs_val_t mjs_get_this(struct mjs *mjs) {
  return mjs->vals.this_obj;
}

static mjs_val_t mjs_get_this_for_js_call(struct mjs *mjs) {
  int top = mjs_stack_size(&mjs->call_stack);
  if (top == 0) return MJS_UNDEFINED;
  assert(top >= 4 && (top % 4) == 0);
  return *vptr(&mjs->call_stack, top - 4);
}

static double do_arith_op(double da, double db, int op) {
  /* clang-format off */
  switch (op) {
    case TOK_MINUS:   return da - db;
    case TOK_PLUS:    return da + db;
    case TOK_MUL:     return da * db;
    case TOK_DIV:     return da / db;
    case TOK_REM:     return (int64_t) da % (int64_t) db;
    case TOK_AND:     return (int64_t) da & (int64_t) db;
    case TOK_OR:      return (int64_t) da | (int64_t) db;
    case TOK_XOR:     return (int64_t) da ^ (int64_t) db;
    case TOK_LSHIFT:  return (int64_t) da << (int64_t) db;
    case TOK_RSHIFT:  return (int64_t) da >> (int64_t) db;
    case TOK_URSHIFT: return (uint32_t) da >> (uint32_t) db;
  }
  /* clang-format on */
  return (int64_t) MJS_TAG_NAN;
}

static void op_assign(struct mjs *mjs, int op) {
  mjs_val_t val = mjs_pop(mjs);
  mjs_val_t obj = mjs_pop(mjs);
  mjs_val_t key = mjs_pop(mjs);
  if (mjs_is_object(obj) && mjs_is_string(key)) {
    mjs_val_t v = mjs_get_v(mjs, obj, key);
    if (mjs_is_number(v) && mjs_is_number(val)) {
      double da = mjs_get_double(mjs, v);
      double db = mjs_get_double(mjs, val);
      double result = do_arith_op(da, db, op);
      mjs_set_v(mjs, obj, key, mjs_mk_number(mjs, result));
    } else if (mjs_is_string(v) && mjs_is_string(val) && (op == TOK_PLUS)) {
      mjs_val_t result = s_concat(mjs, v, val);
      mjs_set_v(mjs, obj, key, result);
    } else {
      mjs_set_v(mjs, obj, key, MJS_UNDEFINED);
      mjs_set_errorf(mjs, MJS_TYPE_ERROR, "invalid operand");
    }
    mjs_push(mjs, v);
  } else {
    mjs_set_errorf(mjs, MJS_TYPE_ERROR, "invalid operand");
  }
}

static void exec_expr(struct mjs *mjs, int op) {
  switch (op) {
    case TOK_DOT:
      break;
    case TOK_MINUS:
    case TOK_PLUS:
    case TOK_MUL:
    case TOK_DIV:
    case TOK_REM:
    case TOK_XOR:
    case TOK_AND:
    case TOK_OR:
    case TOK_LSHIFT:
    case TOK_RSHIFT:
    case TOK_URSHIFT: {
      mjs_val_t a = mjs_pop(mjs);
      mjs_val_t b = mjs_pop(mjs);
      if (mjs_is_number(a) && mjs_is_number(b)) {
        double da = mjs_get_double(mjs, a);
        double db = mjs_get_double(mjs, b);
        mjs_push(mjs, mjs_mk_number(mjs, do_arith_op(db, da, op)));
      } else if (mjs_is_string(a) && mjs_is_string(b) && (op == TOK_PLUS)) {
        mjs_push(mjs, s_concat(mjs, b, a));
      } else {
        mjs_push(mjs, MJS_UNDEFINED);
        mjs_set_errorf(mjs, MJS_TYPE_ERROR, NULL);
      }
      break;
    }
    case TOK_UNARY_MINUS: {
      double a = mjs_get_double(mjs, mjs_pop(mjs));
      mjs_push(mjs, mjs_mk_number(mjs, -a));
      break;
    }
    case TOK_NOT: {
      mjs_val_t val = mjs_pop(mjs);
      mjs_push(mjs, mjs_mk_boolean(mjs, !mjs_is_truthy(mjs, val)));
      break;
    }
    case TOK_TILDA: {
      double a = mjs_get_double(mjs, mjs_pop(mjs));
      mjs_push(mjs, mjs_mk_number(mjs, ~(int64_t) a));
      break;
    }
    case TOK_UNARY_PLUS:
      break;
    case TOK_EQ:
      mjs_set_errorf(mjs, MJS_NOT_IMPLEMENTED_ERROR, "Use ===, not ==");
      break;
    case TOK_NE:
      mjs_set_errorf(mjs, MJS_NOT_IMPLEMENTED_ERROR, "Use !==, not !=");
      break;
    case TOK_EQ_EQ: {
      mjs_val_t a = mjs_pop(mjs);
      mjs_val_t b = mjs_pop(mjs);
      if (a == b) {
        mjs_push(mjs, mjs_mk_boolean(mjs, 1));
      } else if (mjs_is_number(a) && mjs_is_number(b)) {
        mjs_push(mjs, mjs_mk_boolean(mjs, 0));
      } else if (mjs_is_string(a) && mjs_is_string(b)) {
        mjs_push(mjs, mjs_mk_boolean(mjs, s_cmp(mjs, a, b) == 0));
      } else {
        mjs_push(mjs, mjs_mk_boolean(mjs, 0));
      }
      break;
    }
    case TOK_NE_NE: {
      mjs_val_t a = mjs_pop(mjs);
      mjs_val_t b = mjs_pop(mjs);
      if (a == b) {
        mjs_push(mjs, mjs_mk_boolean(mjs, 0));
      } else if (mjs_is_number(a) && mjs_is_number(b)) {
        mjs_push(mjs, mjs_mk_boolean(mjs, 1));
      } else if (mjs_is_string(a) && mjs_is_string(b)) {
        mjs_push(mjs, mjs_mk_boolean(mjs, s_cmp(mjs, a, b) != 0));
      } else {
        mjs_push(mjs, mjs_mk_boolean(mjs, 1));
      }
      break;
    }
    case TOK_LT: {
      double b = mjs_get_double(mjs, mjs_pop(mjs));
      double a = mjs_get_double(mjs, mjs_pop(mjs));
      mjs_push(mjs, mjs_mk_boolean(mjs, a < b));
      break;
    }
    case TOK_GT: {
      double b = mjs_get_double(mjs, mjs_pop(mjs));
      double a = mjs_get_double(mjs, mjs_pop(mjs));
      mjs_push(mjs, mjs_mk_boolean(mjs, a > b));
      break;
    }
    case TOK_LE: {
      double b = mjs_get_double(mjs, mjs_pop(mjs));
      double a = mjs_get_double(mjs, mjs_pop(mjs));
      mjs_push(mjs, mjs_mk_boolean(mjs, a <= b));
      break;
    }
    case TOK_GE: {
      double b = mjs_get_double(mjs, mjs_pop(mjs));
      double a = mjs_get_double(mjs, mjs_pop(mjs));
      mjs_push(mjs, mjs_mk_boolean(mjs, a >= b));
      break;
    }
    case TOK_ASSIGN: {
      mjs_val_t val = mjs_pop(mjs);
      mjs_val_t obj = mjs_pop(mjs);
      mjs_val_t key = mjs_pop(mjs);
      mjs_set_v(mjs, obj, key, val);
      mjs_push(mjs, val);
      break;
    }
    case TOK_POSTFIX_PLUS: {
      mjs_val_t obj = mjs_pop(mjs);
      mjs_val_t key = mjs_pop(mjs);
      if (mjs_is_object(obj) && mjs_is_string(key)) {
        mjs_val_t v = mjs_get_v(mjs, obj, key);
        mjs_val_t v1 = mjs_mk_number(mjs, mjs_get_double(mjs, v) + 1);
        mjs_set_v(mjs, obj, key, v1);
        mjs_push(mjs, v);
      } else {
        mjs_set_errorf(mjs, MJS_TYPE_ERROR, "invalid operand for ++");
      }
      break;
    }
    case TOK_POSTFIX_MINUS: {
      mjs_val_t obj = mjs_pop(mjs);
      mjs_val_t key = mjs_pop(mjs);
      if (mjs_is_object(obj) && mjs_is_string(key)) {
        mjs_val_t v = mjs_get_v(mjs, obj, key);
        mjs_val_t v1 = mjs_mk_number(mjs, mjs_get_double(mjs, v) - 1);
        mjs_set_v(mjs, obj, key, v1);
        mjs_push(mjs, v);
      } else {
        mjs_set_errorf(mjs, MJS_TYPE_ERROR, "invalid operand for --");
      }
      break;
    }
    case TOK_MINUS_MINUS: {
      mjs_val_t obj = mjs_pop(mjs);
      mjs_val_t key = mjs_pop(mjs);
      if (mjs_is_object(obj) && mjs_is_string(key)) {
        mjs_val_t v = mjs_get_v(mjs, obj, key);
        v = mjs_mk_number(mjs, mjs_get_double(mjs, v) - 1);
        mjs_set_v(mjs, obj, key, v);
        mjs_push(mjs, v);
      } else {
        mjs_set_errorf(mjs, MJS_TYPE_ERROR, "invalid operand for --");
      }
      break;
    }
    case TOK_PLUS_PLUS: {
      mjs_val_t obj = mjs_pop(mjs);
      mjs_val_t key = mjs_pop(mjs);
      if (mjs_is_object(obj) && mjs_is_string(key)) {
        mjs_val_t v = mjs_get_v(mjs, obj, key);
        v = mjs_mk_number(mjs, mjs_get_double(mjs, v) + 1);
        mjs_set_v(mjs, obj, key, v);
        mjs_push(mjs, v);
      } else {
        mjs_set_errorf(mjs, MJS_TYPE_ERROR, "invalid operand for ++");
      }
      break;
    }
    case TOK_LOGICAL_AND: {
      mjs_val_t b = mjs_pop(mjs);
      mjs_val_t a = mjs_pop(mjs);
      mjs_push(mjs, mjs_is_truthy(mjs, a) ? b : a);
      break;
    }
    case TOK_LOGICAL_OR: {
      mjs_val_t b = mjs_pop(mjs);
      mjs_val_t a = mjs_pop(mjs);
      mjs_push(mjs, mjs_is_truthy(mjs, a) ? a : b);
      break;
    }
    /* clang-format off */
    case TOK_MINUS_ASSIGN:    op_assign(mjs, TOK_MINUS);    break;
    case TOK_PLUS_ASSIGN:     op_assign(mjs, TOK_PLUS);     break;
    case TOK_MUL_ASSIGN:      op_assign(mjs, TOK_MUL);      break;
    case TOK_DIV_ASSIGN:      op_assign(mjs, TOK_DIV);      break;
    case TOK_REM_ASSIGN:      op_assign(mjs, TOK_REM);      break;
    case TOK_AND_ASSIGN:      op_assign(mjs, TOK_AND);      break;
    case TOK_OR_ASSIGN:       op_assign(mjs, TOK_OR);       break;
    case TOK_XOR_ASSIGN:      op_assign(mjs, TOK_XOR);      break;
    case TOK_LSHIFT_ASSIGN:   op_assign(mjs, TOK_LSHIFT);   break;
    case TOK_RSHIFT_ASSIGN:   op_assign(mjs, TOK_RSHIFT);   break;
    case TOK_URSHIFT_ASSIGN:  op_assign(mjs, TOK_URSHIFT);  break;
    case TOK_COMMA: break;
    /* clang-format on */
    case TOK_KEYWORD_TYPEOF:
      mjs_push(mjs, mjs_mk_string(mjs, mjs_typeof(mjs_pop(mjs)), ~0, 1));
      break;
    default:
      LOG(LL_ERROR, ("Unknown expr: %d", op));
      break;
  }
}

static void mjs_execute(struct mjs *mjs, size_t off) {
  size_t i;
  mjs_set_errorf(mjs, MJS_OK, NULL);
  for (i = off; i < mjs->bcode.len; i++) {
    if (mjs->need_gc) {
      if (maybe_gc(mjs)) {
        mjs->need_gc = 0;
      }
    }
#if MJS_AGGRESSIVE_GC
    maybe_gc(mjs);
#endif
    const uint8_t *code = (const uint8_t *) mjs->bcode.buf;
    if (cs_log_level >= LL_VERBOSE_DEBUG) {
      /* mjs_dump(mjs, 0, stdout); */
      printf("executing: ");
      mjs_disasm_single(code, i, stdout);
    }
    switch (code[i]) {
      case OP_BCODE_HEADER: {
        mjs_header_item_t bcode_offset;
        memcpy(&bcode_offset,
               code + i + 1 +
                   sizeof(mjs_header_item_t) * MJS_HDR_ITEM_BCODE_OFFSET,
               sizeof(bcode_offset));
        i += bcode_offset;
      } break;
      case OP_PUSH_NULL:
        mjs_push(mjs, mjs_mk_null());
        break;
      case OP_PUSH_UNDEF:
        mjs_push(mjs, mjs_mk_undefined());
        break;
      case OP_PUSH_FALSE:
        mjs_push(mjs, mjs_mk_boolean(mjs, 0));
        break;
      case OP_PUSH_TRUE:
        mjs_push(mjs, mjs_mk_boolean(mjs, 1));
        break;
      case OP_PUSH_OBJ:
        mjs_push(mjs, mjs_mk_object(mjs));
        break;
      case OP_PUSH_ARRAY:
        mjs_push(mjs, mjs_mk_array(mjs));
        break;
      case OP_PUSH_FUNC: {
        int llen, n = varint_decode(&code[i + 1], &llen);
        mjs_push(mjs, mjs_mk_function(mjs, i - n));
        i += llen;
        break;
      }
      case OP_PUSH_THIS:
        mjs_push(mjs, mjs_get_this_for_js_call(mjs));
        break;
      case OP_JMP: {
        int llen, n = varint_decode(&code[i + 1], &llen);
        i += n + llen;
        break;
      }
      case OP_JMP_FALSE: {
        int llen, n = varint_decode(&code[i + 1], &llen);
        i += llen;
        if (!mjs_is_truthy(mjs, mjs_pop(mjs))) {
          mjs_push(mjs, MJS_UNDEFINED);
          i += n;
        }
        break;
      }
      case OP_FIND_SCOPE: {
        mjs_val_t key = vtop(&mjs->stack);
        mjs_push(mjs, mjs_find_scope(mjs, key));
        break;
      }
      case OP_CREATE: {
        mjs_val_t obj = mjs_pop(mjs);
        mjs_val_t key = mjs_pop(mjs);
        if (mjs_get_own_property_v(mjs, obj, key) == NULL) {
          mjs_set_v(mjs, obj, key, MJS_UNDEFINED);
        }
        break;
      }
      case OP_APPEND: {
        mjs_val_t val = mjs_pop(mjs);
        mjs_val_t arr = mjs_pop(mjs);
        mjs_err_t err = mjs_array_push(mjs, arr, val);
        if (err != MJS_OK) {
          mjs_set_errorf(mjs, MJS_TYPE_ERROR, "append to non-array");
        }
        break;
      }
      case OP_SET_THIS:
        mjs->vals.this_obj = vtop(&mjs->stack);
        break;
      case OP_GET: {
        mjs_val_t obj = mjs_pop(mjs);
        mjs_val_t key = mjs_pop(mjs);
        mjs_val_t val = MJS_UNDEFINED;
        if (!mjs_is_object(obj)) {
          mjs_set_errorf(mjs, MJS_TYPE_ERROR, "type error");
        } else {
          val = mjs_get_v_proto(mjs, obj, key);
        }
        mjs_push(mjs, val);
        break;
      }
      case OP_DEL_SCOPE:
        if (mjs->scopes.len <= 1) {
          mjs_set_errorf(mjs, MJS_INTERNAL_ERROR, "scopes underflow");
        } else {
          mjs_pop_val(&mjs->scopes);
        }
        break;
      case OP_NEW_SCOPE:
        push_mjs_val(&mjs->scopes, mjs_mk_object(mjs));
        break;
      case OP_PUSH_SCOPE:
        assert(mjs_stack_size(&mjs->scopes) > 0);
        mjs_push(mjs, vtop(&mjs->scopes));
        break;
      case OP_PUSH_STR: {
        int llen, n = varint_decode(&code[i + 1], &llen);
        mjs_push(mjs, mjs_mk_string(mjs, (char *) code + i + 1 + llen, n, 1));
        i += llen + n;
        break;
      }
      case OP_PUSH_INT: {
        int llen;
        int64_t n = varint_decode(&code[i + 1], &llen);
        mjs_push(mjs, mjs_mk_number(mjs, n));
        i += llen;
        break;
      }
      case OP_PUSH_DBL: {
        int llen, n = varint_decode(&code[i + 1], &llen);
        mjs_push(mjs, mjs_mk_number(
                          mjs, strtod((char *) code + i + 1 + llen, NULL)));
        i += llen + n;
        break;
      }
      case OP_FOR_IN_NEXT: {
        /*
         * Data stack layout:
         * ...                                    <-- Bottom of the data stack
         * <iterator_variable_name>   (string)
         * <object_that_is_iterated>  (object)
         * <iterator_foreign_ptr>                 <-- Top of the data stack
         */
        mjs_val_t *iterator = vptr(&mjs->stack, -1);
        mjs_val_t obj = *vptr(&mjs->stack, -2);
        mjs_val_t var_name = *vptr(&mjs->stack, -3);
        mjs_val_t key = mjs_next(mjs, obj, iterator);
        if (key != MJS_UNDEFINED) {
          mjs_val_t scope = mjs_find_scope(mjs, var_name);
          mjs_set_v(mjs, scope, var_name, key);
        }
        break;
      }
      case OP_RETURN: {
        size_t retval_stack_idx, return_address, scope_index;
        assert(mjs_stack_size(&mjs->call_stack) >= 4);

        retval_stack_idx = mjs_get_int(mjs, mjs_pop_val(&mjs->call_stack));
        scope_index = mjs_get_int(mjs, mjs_pop_val(&mjs->call_stack));
        return_address = mjs_get_int(mjs, mjs_pop_val(&mjs->call_stack));
        mjs_pop_val(&mjs->call_stack);  //  Remove `this` value
        mjs->vals.this_obj = MJS_UNDEFINED;

        // Remove created scopes
        while (mjs_stack_size(&mjs->scopes) > scope_index) {
          mjs_pop_val(&mjs->scopes);
        }

        // Shrink stack, leave return value on top
        mjs->stack.len = retval_stack_idx * sizeof(mjs_val_t);

        // Jump to the return address
        i = return_address - 1;
        LOG(LL_VERBOSE_DEBUG, ("RETURNING TO %d", i + 1));
        // mjs_dump(mjs, 0, stdout);
        break;
      }
      case OP_ARGS: {
        push_mjs_val(&mjs->call_stack,
                     mjs_mk_number(mjs, mjs_stack_size(&mjs->stack)));
        break;
      }
      case OP_CALL: {
        // LOG(LL_INFO, ("BEFORE CALL"));
        // mjs_dump(mjs, 0, stdout);
        mjs_val_t retpos = vtop(&mjs->call_stack);
        int func_pos = mjs_get_int(mjs, retpos) - 1;
        mjs_val_t *func = vptr(&mjs->stack, func_pos);
        if (mjs_is_function(*func)) {
          mjs_pop_val(&mjs->call_stack);
          push_mjs_val(&mjs->call_stack, mjs->vals.this_obj);
          push_mjs_val(&mjs->call_stack, mjs_mk_number(mjs, i + 1));
          push_mjs_val(&mjs->call_stack,
                       mjs_mk_number(mjs, mjs_stack_size(&mjs->scopes)));
          push_mjs_val(&mjs->call_stack, retpos);
          i = mjs_get_func_addr(*func) - 1;
          *func = MJS_UNDEFINED;  // Return value
          // LOG(LL_VERBOSE_DEBUG, ("CALLING  %d", i + 1));
        } else if (mjs_is_string(*func)) {
          mjs_ffi_call2(mjs);
          /* Pop the value pushed by OP_ARGS */
          mjs_pop_val(&mjs->call_stack);
        } else if (mjs_is_foreign(*func)) {
          /* Call cfunction */
          ((void (*) (struct mjs *)) mjs_get_ptr(mjs, *func))(mjs);
          /* Pop the value pushed by OP_ARGS */
          mjs_pop_val(&mjs->call_stack);
        } else {
          mjs_set_errorf(mjs, MJS_TYPE_ERROR, "calling non-callable");
        }
        break;
      }
      case OP_SET_ARG: {
        int llen1, llen2, n, arg_no = varint_decode(&code[i + 1], &llen1);
        n = varint_decode(&code[i + llen1 + 1], &llen2);
        mjs_val_t key =
            mjs_mk_string(mjs, (char *) code + i + 1 + llen1 + llen2, n, 1);
        mjs_val_t obj = vtop(&mjs->scopes);
        mjs_val_t v = mjs_arg(mjs, arg_no);
        mjs_set_v(mjs, obj, key, v);
        i += llen1 + llen2 + n;
        break;
      }
      case OP_SETRETVAL: {
        if (mjs_stack_size(&mjs->call_stack) < 4) {
          mjs_set_errorf(mjs, MJS_INTERNAL_ERROR, "cannot return");
        } else {
          size_t retval_pos = mjs_get_int(mjs, *vptr(&mjs->call_stack, -1));
          *vptr(&mjs->stack, retval_pos - 1) = mjs_pop(mjs);
        }
        // LOG(LL_INFO, ("AFTER SETRETVAL"));
        // mjs_dump(mjs, 0, stdout);
        break;
      }
      case OP_EXPR: {
        int op = code[i + 1];
        exec_expr(mjs, op);
        i++;
        break;
      }
      case OP_DROP: {
        mjs_pop(mjs);
        break;
      }
      case OP_DUP: {
        mjs_push(mjs, vtop(&mjs->stack));
        break;
      }
      case OP_SWAP: {
        mjs_val_t a = mjs_pop(mjs);
        mjs_val_t b = mjs_pop(mjs);
        mjs_push(mjs, a);
        mjs_push(mjs, b);
        break;
      }
      case OP_LOOP: {
        int l1, l2, off = varint_decode(&code[i + 1], &l1);
        push_mjs_val(&mjs->loop_addresses,
                     mjs_mk_number(mjs, i + 1 /* OP_LOOP */ + l1 + off));
        off = varint_decode(&code[i + 1 + l1], &l2);
        push_mjs_val(&mjs->loop_addresses,
                     mjs_mk_number(mjs, i + 1 /* OP_LOOP*/ + l1 + l2 + off));
        i += l1 + l2;
        break;
      }
      case OP_CONTINUE:
        i = mjs_get_int(mjs, vtop(&mjs->loop_addresses)) - 1;
        break;
      case OP_BREAK:
        mjs_pop_val(&mjs->loop_addresses);
        i = mjs_get_int(mjs, mjs_pop_val(&mjs->loop_addresses)) - 1;
        LOG(LL_VERBOSE_DEBUG, ("BREAKING TO %d", (int) i + 1));
        break;
      case OP_NOP:
        break;
      case OP_EXIT:
        i = mjs->bcode.len;
        break;
      default:
#if MJS_ENABLE_DEBUG
        mjs_dump(mjs, 1, stdout);
#endif
        mjs_set_errorf(mjs, MJS_INTERNAL_ERROR, "Unknown opcode: %d, off %d",
                       (int) code[i], (int) i);
        i = mjs->bcode.len;
        break;
    }
    if (mjs->error != MJS_OK) {
      mjs_print_stack_trace(mjs, i - 1 /* undo the i++ */);
      break;
    }
  }
}

mjs_err_t mjs_exec2(struct mjs *mjs, const char *path, const char *src,
                    mjs_val_t *res) {
  size_t off = mjs->bcode.len;
  mjs_val_t r = MJS_UNDEFINED;
  mjs->error = mjs_parse(path, src, mjs);
  if (cs_log_level >= LL_VERBOSE_DEBUG) mjs_dump(mjs, 1, stderr);
  if (mjs->error != MJS_OK) {
  } else {
    mjs_execute(mjs, off);
    r = mjs_pop(mjs);
  }
  if (res != NULL) *res = r;
  return mjs->error;
}

mjs_err_t mjs_exec(struct mjs *mjs, const char *src, mjs_val_t *res) {
  return mjs_exec2(mjs, "<stdin>", src, res);
}

mjs_err_t mjs_exec_file(struct mjs *mjs, const char *path, mjs_val_t *res) {
  mjs_err_t error = MJS_FILE_READ_ERROR;
  mjs_val_t r = MJS_UNDEFINED;
  size_t size;
  char *source_code = cs_read_file(path, &size);
  r = MJS_UNDEFINED;
  if (source_code != NULL) {
    error = mjs_exec2(mjs, path, source_code, &r);
    free(source_code);
  }
  if (res != NULL) *res = r;
  return error;
}

mjs_err_t mjs_call(struct mjs *mjs, mjs_val_t *res, mjs_val_t func,
                   mjs_val_t this_val, int nargs, ...) {
  va_list ap;
  int i;
  mjs_err_t ret;
  va_start(ap, nargs);
  mjs_val_t *args = calloc(1, sizeof(mjs_val_t) * nargs);
  for (i = 0; i < nargs; i++) {
    args[i] = va_arg(ap, mjs_val_t);
  }

  ret = mjs_apply(mjs, res, func, this_val, nargs, args);
  /*
   * NOTE: calling `bf_run()` invalidates `func` and `this_val`. If you ever
   * need to use them afterwards, you need to own them before.
   */

  free(args);
  return ret;
}

mjs_err_t mjs_apply(struct mjs *mjs, mjs_val_t *res, mjs_val_t func,
                    mjs_val_t this_val, int nargs, mjs_val_t *args) {
  mjs_val_t r;
  int i;
  size_t addr = mjs_get_func_addr(func);

  LOG(LL_VERBOSE_DEBUG, ("applying func %d", mjs_get_func_addr(func)));

  push_mjs_val(&mjs->call_stack, this_val);

  // Push return address: returning to the end of bcode causes execution
  // to stop
  // TODO(dfrank): make special return value for that, because mjs->bcode.len
  // might change after function invocation (if function parses some new code)
  push_mjs_val(&mjs->call_stack, mjs_mk_number(mjs, mjs->bcode.len));
  push_mjs_val(&mjs->call_stack,
               mjs_mk_number(mjs, mjs_stack_size(&mjs->scopes)));
  // Push undefined which will be later replaced with the return value
  mjs_push(mjs, MJS_UNDEFINED);
  push_mjs_val(&mjs->call_stack,
               mjs_mk_number(mjs, mjs_stack_size(&mjs->stack)));

  // Push all arguments
  for (i = 0; i < nargs; i++) {
    mjs_push(mjs, args[i]);
  }

  mjs_execute(mjs, addr);
  r = mjs_pop(mjs);

  if (res != NULL) *res = r;
  return mjs->error;
}
