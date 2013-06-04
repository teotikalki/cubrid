/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */


/*
 * fetch.c - Object/Tuple value fetch routines
 */

#ident "$Id$"

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#if !defined(WINDOWS)
#include <sys/time.h>
#endif

#include "porting.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "db.h"
#include "storage_common.h"
#include "memory_alloc.h"
#include "oid.h"
#include "object_primitive.h"
#include "object_representation.h"
#include "arithmetic.h"
#include "serial.h"
#include "session.h"
#include "fetch.h"
#include "list_file.h"
#include "string_opfunc.h"
#include "server_interface.h"

/* this must be the last header file included!!! */
#include "dbval.h"

static int fetch_peek_arith (THREAD_ENTRY * thread_p,
			     REGU_VARIABLE * regu_var, VAL_DESCR * vd,
			     OID * obj_oid, QFILE_TUPLE tpl,
			     DB_VALUE ** peek_dbval);
static int fetch_peek_dbval_pos (REGU_VARIABLE * regu_var, QFILE_TUPLE tpl,
				 int pos, DB_VALUE ** peek_dbval,
				 QFILE_TUPLE * next_tpl);
static int fetch_peek_min_max_value_of_width_bucket_func (THREAD_ENTRY *
							  thread_p,
							  REGU_VARIABLE *
							  regu_var,
							  VAL_DESCR * vd,
							  OID * obj_oid,
							  QFILE_TUPLE tpl,
							  DB_VALUE ** min,
							  DB_VALUE ** max);

static bool is_argument_wrapped_with_cast_op (const REGU_VARIABLE * regu_var);

/*
 * fetch_peek_arith () -
 *   return: NO_ERROR or ER_code
 *   regu_var(in/out): Regulator Variable of an ARITH node.
 *   vd(in): Value Descriptor
 *   obj_oid(in): Object Identifier
 *   tpl(in): Tuple
 *   peek_dbval(out): Set to the value resulting from the fetch operation
 */
static int
fetch_peek_arith (THREAD_ENTRY * thread_p, REGU_VARIABLE * regu_var,
		  VAL_DESCR * vd, OID * obj_oid, QFILE_TUPLE tpl,
		  DB_VALUE ** peek_dbval)
{
  ARITH_TYPE *arithptr = regu_var->value.arithptr;
  DB_VALUE *peek_left, *peek_right, *peek_third, *peek_fourth;
  DB_VALUE tmp_value;
  TP_DOMAIN *original_domain = NULL;
  TP_DOMAIN_STATUS dom_status;

  peek_left = NULL;
  peek_right = NULL;
  peek_third = NULL;
  peek_fourth = NULL;

  /* fetch values */
  switch (arithptr->opcode)
    {
    case T_SUBSTRING:
    case T_LPAD:
    case T_RPAD:
    case T_REPLACE:
    case T_TRANSLATE:
    case T_TO_CHAR:
    case T_TO_DATE:
    case T_TO_TIME:
    case T_TO_TIMESTAMP:
    case T_TO_DATETIME:
    case T_TO_NUMBER:
    case T_INSTR:
    case T_MID:
    case T_DATE_ADD:
    case T_DATE_SUB:
    case T_NEXT_VALUE:

      /* fetch lhs, rhs, and third value */
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      if (!DB_IS_NULL (peek_left))
	{
	  if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				vd, NULL, obj_oid, tpl,
				&peek_right) != NO_ERROR)
	    {
	      goto error;
	    }
	  if ((prm_get_integer_value (PRM_ID_COMPAT_MODE) == COMPAT_MYSQL
	       || arithptr->opcode == T_SUBSTRING) && !arithptr->thirdptr)
	    {
	      break;
	    }
	  if (fetch_peek_dbval (thread_p, arithptr->thirdptr,
				vd, NULL, obj_oid, tpl,
				&peek_third) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_STR_TO_DATE:
    case T_DATE_FORMAT:
    case T_TIME_FORMAT:
    case T_FORMAT:
      if (fetch_peek_dbval (thread_p, arithptr->thirdptr,
			    vd, NULL, obj_oid, tpl, &peek_third) != NO_ERROR)
	{
	  goto error;
	}
    case T_ADD:
    case T_SUB:
    case T_MUL:
    case T_DIV:
    case T_MOD:
    case T_POSITION:
    case T_FINDINSET:
    case T_ADD_MONTHS:
    case T_MONTHS_BETWEEN:
    case T_TRIM:
    case T_AES_ENCRYPT:
    case T_AES_DECRYPT:
    case T_SHA_TWO:
    case T_LTRIM:
    case T_RTRIM:
    case T_POWER:
    case T_ROUND:
    case T_LOG:
    case T_TRUNC:
    case T_STRCAT:
    case T_NULLIF:
    case T_INCR:
    case T_DECR:
    case T_BIT_AND:
    case T_BIT_OR:
    case T_BIT_XOR:
    case T_BITSHIFT_LEFT:
    case T_BITSHIFT_RIGHT:
    case T_INTDIV:
    case T_INTMOD:
    case T_STRCMP:
    case T_ATAN2:
    case T_ADDDATE:
    case T_SUBDATE:
    case T_DATEDIFF:
    case T_TIMEDIFF:
    case T_CURRENT_VALUE:
    case T_CHR:
      /* fetch lhs and rhs value */
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      if (DB_IS_NULL (peek_left))
	{
	  if (prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING)
	      && (arithptr->opcode == T_STRCAT || arithptr->opcode == T_ADD))
	    {
	      /* check for result type. */
	      if (TP_DOMAIN_TYPE (regu_var->domain) == DB_TYPE_VARIABLE
		  ||
		  QSTR_IS_ANY_CHAR_OR_BIT (TP_DOMAIN_TYPE (regu_var->domain)))
		{
		  if (fetch_peek_dbval (thread_p, arithptr->rightptr,
					vd, NULL, obj_oid, tpl,
					&peek_right) != NO_ERROR)
		    {
		      goto error;
		    }
		}
	    }
	}
      else
	{
	  if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				vd, NULL, obj_oid, tpl,
				&peek_right) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_FROM_UNIXTIME:

      if (fetch_peek_dbval (thread_p, arithptr->leftptr, vd, NULL, obj_oid,
			    tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      if (arithptr->rightptr != NULL)
	{
	  if (fetch_peek_dbval (thread_p, arithptr->rightptr, vd, NULL,
				obj_oid, tpl, &peek_right) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_SUBSTRING_INDEX:
    case T_CONCAT_WS:
    case T_FIELD:
    case T_INDEX_CARDINALITY:
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      if (arithptr->rightptr != NULL)
	{
	  if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				vd, NULL, obj_oid, tpl,
				&peek_right) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      if (fetch_peek_dbval (thread_p, arithptr->thirdptr,
			    vd, NULL, obj_oid, tpl, &peek_third) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_CONV:
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      if (fetch_peek_dbval (thread_p, arithptr->rightptr,
			    vd, NULL, obj_oid, tpl, &peek_right) != NO_ERROR)
	{
	  goto error;
	}
      if (fetch_peek_dbval (thread_p, arithptr->thirdptr,
			    vd, NULL, obj_oid, tpl, &peek_third) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_LOCATE:
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      if (fetch_peek_dbval (thread_p, arithptr->rightptr,
			    vd, NULL, obj_oid, tpl, &peek_right) != NO_ERROR)
	{
	  goto error;
	}
      if (arithptr->thirdptr != NULL)
	{
	  if (fetch_peek_dbval (thread_p, arithptr->thirdptr,
				vd, NULL, obj_oid, tpl,
				&peek_third) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_CONCAT:
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      if (DB_IS_NULL (peek_left))
	{
	  if (prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING))
	    {
	      if (arithptr->rightptr != NULL)
		{
		  if (fetch_peek_dbval (thread_p, arithptr->rightptr,
					vd, NULL, obj_oid, tpl,
					&peek_right) != NO_ERROR)
		    {
		      goto error;
		    }
		}
	    }
	}
      else
	{
	  if (arithptr->rightptr != NULL)
	    {
	      if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				    vd, NULL, obj_oid, tpl,
				    &peek_right) != NO_ERROR)
		{
		  goto error;
		}
	    }
	}
      break;

    case T_REPEAT:
    case T_LEAST:
    case T_GREATEST:
    case T_SYS_CONNECT_BY_PATH:
    case T_LEFT:
    case T_RIGHT:
      /* fetch both lhs and rhs value */
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      if (fetch_peek_dbval (thread_p, arithptr->rightptr,
			    vd, NULL, obj_oid, tpl, &peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_MAKEDATE:
    case T_ADDTIME:
    case T_WEEK:
    case T_DEFINE_VARIABLE:
      /* fetch both lhs and rhs value */
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  if (is_argument_wrapped_with_cast_op (arithptr->leftptr))
	    {
	      /* cast might have failed: set ER_DATE_CONVERSION */
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_DATE_CONVERSION,
		      0);
	    }
	  goto error;
	}
      if (fetch_peek_dbval (thread_p, arithptr->rightptr,
			    vd, NULL, obj_oid, tpl, &peek_right) != NO_ERROR)
	{
	  if (is_argument_wrapped_with_cast_op (arithptr->rightptr))
	    {
	      /* cast might have failed: set ER_DATE_CONVERSION */
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_DATE_CONVERSION,
		      0);
	    }
	  goto error;
	}
      break;

    case T_MAKETIME:
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  if (is_argument_wrapped_with_cast_op (arithptr->leftptr))
	    {
	      /* cast might have failed: set ER_TIME_CONVERSION */
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_TIME_CONVERSION,
		      0);
	    }
	  goto error;
	}
      if (fetch_peek_dbval (thread_p, arithptr->rightptr,
			    vd, NULL, obj_oid, tpl, &peek_right) != NO_ERROR)
	{
	  if (is_argument_wrapped_with_cast_op (arithptr->rightptr))
	    {
	      /* cast might have failed: set ER_TIME_CONVERSION */
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_TIME_CONVERSION,
		      0);
	    }
	  goto error;
	}
      if (fetch_peek_dbval (thread_p, arithptr->thirdptr,
			    vd, NULL, obj_oid, tpl, &peek_third) != NO_ERROR)
	{
	  if (is_argument_wrapped_with_cast_op (arithptr->thirdptr))
	    {
	      /* cast might have failed: set ER_TIME_CONVERSION */
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_TIME_CONVERSION,
		      0);
	    }
	  goto error;
	}
      break;

    case T_CASE:
    case T_DECODE:
    case T_IF:
    case T_IFNULL:
    case T_NVL:
    case T_PREDICATE:
    case T_COALESCE:
    case T_NVL2:
      /* defer fetch values */
      break;

    case T_LAST_DAY:
#if defined(ENABLE_UNUSED_FUNCTION)
    case T_UNPLUS:
#endif /* ENABLE_UNUSED_FUNCTION */
    case T_UNMINUS:
    case T_OCTET_LENGTH:
    case T_BIT_LENGTH:
    case T_CHAR_LENGTH:
    case T_LOWER:
    case T_UPPER:
    case T_HEX:
    case T_ASCII:
    case T_SPACE:
    case T_MD5:
    case T_SHA_ONE:
    case T_BIN:
    case T_CAST:
    case T_CAST_NOFAIL:
    case T_EXTRACT:
    case T_FLOOR:
    case T_CEIL:
    case T_SIGN:
    case T_ABS:
    case T_EXP:
    case T_SQRT:
    case T_PRIOR:
    case T_BIT_NOT:
    case T_REVERSE:
    case T_BIT_COUNT:
    case T_ACOS:
    case T_ASIN:
    case T_SIN:
    case T_COS:
    case T_TAN:
    case T_COT:
    case T_DEGREES:
    case T_RADIANS:
    case T_LN:
    case T_LOG2:
    case T_LOG10:
    case T_ATAN:
    case T_DATE:
    case T_TIME:
    case T_ISNULL:
    case T_RAND:
    case T_DRAND:
    case T_RANDOM:
    case T_DRANDOM:
    case T_TYPEOF:
    case T_EXEC_STATS:
    case T_INET_ATON:
    case T_INET_NTOA:
    case T_CHARSET:
    case T_COLLATION:
      /* fetch rhs value */
      if (fetch_peek_dbval (thread_p, arithptr->rightptr,
			    vd, NULL, obj_oid, tpl, &peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_YEAR:
    case T_MONTH:
    case T_DAY:
    case T_QUARTER:
    case T_WEEKDAY:
    case T_DAYOFWEEK:
    case T_DAYOFYEAR:
    case T_TODAYS:
    case T_FROMDAYS:
    case T_EVALUATE_VARIABLE:
      /* fetch rhs value */
      if (fetch_peek_dbval (thread_p, arithptr->rightptr,
			    vd, NULL, obj_oid, tpl, &peek_right) != NO_ERROR)
	{
	  if (is_argument_wrapped_with_cast_op (arithptr->rightptr))
	    {
	      /* cast might have failed: set ER_DATE_CONVERSION */
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_DATE_CONVERSION,
		      0);
	    }
	  goto error;
	}
      break;

    case T_HOUR:
    case T_MINUTE:
    case T_SECOND:
    case T_TIMETOSEC:
    case T_SECTOTIME:
      /* fetch rhs value */
      if (fetch_peek_dbval (thread_p, arithptr->rightptr,
			    vd, NULL, obj_oid, tpl, &peek_right) != NO_ERROR)
	{
	  if (is_argument_wrapped_with_cast_op (arithptr->rightptr))
	    {
	      /* if cast failed, set ER_TIME_CONVERSION */
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_TIME_CONVERSION,
		      0);
	    }
	  goto error;
	}
      break;

    case T_UNIX_TIMESTAMP:
    case T_DEFAULT:
      if (arithptr->rightptr)
	{
	  if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				vd, NULL, obj_oid, tpl,
				&peek_right) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_TIMESTAMP:
    case T_LIKE_LOWER_BOUND:
    case T_LIKE_UPPER_BOUND:
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      if (arithptr->rightptr)
	{
	  if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				vd, NULL, obj_oid, tpl,
				&peek_right) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_CONNECT_BY_ROOT:
    case T_QPRIOR:
    case T_SYS_DATE:
    case T_SYS_TIME:
    case T_SYS_TIMESTAMP:
    case T_SYS_DATETIME:
    case T_UTC_TIME:
    case T_UTC_DATE:
    case T_LOCAL_TRANSACTION_ID:
    case T_PI:
    case T_ROW_COUNT:
    case T_LAST_INSERT_ID:
    case T_LIST_DBS:
      /* nothing to fetch */
      break;

    case T_BLOB_TO_BIT:
    case T_CLOB_TO_CHAR:
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      if (!DB_IS_NULL (peek_left) && arithptr->rightptr)
	{
	  if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				vd, NULL, obj_oid, tpl,
				&peek_right) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_BIT_TO_BLOB:
    case T_CHAR_TO_CLOB:
    case T_LOB_LENGTH:
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TO_ENUMERATION_VALUE:
      if (fetch_peek_dbval (thread_p, arithptr->rightptr,
			    vd, NULL, obj_oid, tpl, &peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_WIDTH_BUCKET:
      if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			    vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	{
	  goto error;
	}

      /* get peek_righ, peed_third
       * we use PT_BETWEEN with PT_BETWEEN_GE_LT to represent the two args.
       */
      if (fetch_peek_min_max_value_of_width_bucket_func (thread_p,
							 arithptr->rightptr,
							 vd,
							 obj_oid,
							 tpl,
							 &peek_right,
							 &peek_third) !=
	  NO_ERROR)
	{
	  goto error;
	}

      if (fetch_peek_dbval (thread_p, arithptr->thirdptr,
			    vd, NULL, obj_oid, tpl, &peek_fourth) != NO_ERROR)
	{
	  goto error;
	}
      break;

    default:
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_QPROC_INVALID_XASLNODE, 0);
      goto error;
    }

  /* evaluate arithmetic expression */

  /* clear any previous result */
  pr_clear_value (arithptr->value);
  if (regu_var->domain != NULL
      && TP_DOMAIN_TYPE (regu_var->domain) == DB_TYPE_VARIABLE)
    {
      original_domain = regu_var->domain;
      regu_var->domain = NULL;
    }
  switch (arithptr->opcode)
    {
    case T_ADD:
      {
	bool check_empty_string;

	check_empty_string =
	  (prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING) ? true :
	   false);

	/* check for result type. */
	if (check_empty_string
	    && regu_var->domain != NULL
	    && QSTR_IS_ANY_CHAR_OR_BIT (TP_DOMAIN_TYPE (regu_var->domain)))
	  {
	    /* at here, T_ADD is really T_STRCAT */
	    if (qdata_strcat_dbval (peek_left, peek_right,
				    arithptr->value,
				    regu_var->domain) != NO_ERROR)
	      {
		goto error;
	      }
	  }
	else
	  {
	    if (qdata_add_dbval (peek_left, peek_right,
				 arithptr->value,
				 regu_var->domain) != NO_ERROR)
	      {
		goto error;
	      }
	  }
      }
      break;

    case T_BIT_NOT:
      if (qdata_bit_not_dbval (peek_right, arithptr->value,
			       regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_BIT_AND:
      if (qdata_bit_and_dbval (peek_left, peek_right, arithptr->value,
			       regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_BIT_OR:
      if (qdata_bit_or_dbval (peek_left, peek_right, arithptr->value,
			      regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_BIT_XOR:
      if (qdata_bit_xor_dbval (peek_left, peek_right, arithptr->value,
			       regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_BITSHIFT_LEFT:
    case T_BITSHIFT_RIGHT:
      if (qdata_bit_shift_dbval (peek_left, peek_right, arithptr->opcode,
				 arithptr->value,
				 regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_INTDIV:
    case T_INTMOD:
      if (qdata_divmod_dbval (peek_left, peek_right, arithptr->opcode,
			      arithptr->value, regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_BIT_COUNT:
      if (db_bit_count_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_SUB:
      if (qdata_subtract_dbval (peek_left, peek_right,
				arithptr->value,
				regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_MUL:
      if (qdata_multiply_dbval (peek_left, peek_right,
				arithptr->value,
				regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DIV:
      if (qdata_divide_dbval (peek_left, peek_right,
			      arithptr->value, regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

#if defined(ENABLE_UNUSED_FUNCTION)
    case T_UNPLUS:
      if (!qdata_copy_db_value (arithptr->value, peek_right))
	{
	  goto error;
	}
      break;
#endif
    case T_UNPLUS:
      break;


    case T_UNMINUS:
      if (qdata_unary_minus_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_PRIOR:
      if (!qdata_copy_db_value (arithptr->value, peek_right))
	{
	  goto error;
	}
      break;

    case T_CONNECT_BY_ROOT:
      if (!qdata_evaluate_connect_by_root (thread_p,
					   (void *) arithptr->thirdptr->xasl,
					   arithptr->rightptr,
					   arithptr->value, vd))
	{
	  goto error;
	}
      break;

    case T_QPRIOR:
      if (!qdata_evaluate_qprior (thread_p,
				  (void *) arithptr->thirdptr->xasl,
				  arithptr->rightptr, arithptr->value, vd))
	{
	  goto error;
	}
      break;

    case T_MOD:
      if (db_mod_dbval (arithptr->value, peek_left, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_FLOOR:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_floor_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_CEIL:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_ceil_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_SIGN:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_sign_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_ABS:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_abs_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_EXP:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_exp_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_SQRT:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_sqrt_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_SIN:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_sin_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_COS:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_cos_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TAN:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_tan_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_COT:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_cot_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_LN:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_log_generic_dbval (arithptr->value, peek_right,
				     -1 /* convention for e base */ ) !=
	       NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_LOG2:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_log_generic_dbval (arithptr->value, peek_right, 2) !=
	       NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_LOG10:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_log_generic_dbval (arithptr->value, peek_right, 10) !=
	       NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_ACOS:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_acos_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_ASIN:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_asin_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DEGREES:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_degrees_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DATE:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_date_dbval (arithptr->value, peek_right, arithptr->domain)
	       != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TIME:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_time_dbval (arithptr->value, peek_right, arithptr->domain)
	       != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DEFAULT:
      qdata_copy_db_value (arithptr->value, peek_right);
      break;

    case T_RADIANS:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_radians_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_POWER:
      if (db_power_dbval (arithptr->value, peek_left, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_ROUND:
      if (db_round_dbval (arithptr->value, peek_left, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_LOG:
      if (db_log_dbval (arithptr->value, peek_left, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_INCR:
    case T_DECR:
      if (DB_IS_NULL (peek_right))
	{
	  /* an instance does not exist to do increment */
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  /*
	   * right argument is used at end of scan,
	   * so keep it in the expr until that.
	   */
	  (void) pr_clone_value (peek_right, arithptr->value);
	  *peek_dbval = peek_left;
	  return NO_ERROR;
	}
      break;

    case T_TRUNC:
      if (db_trunc_dbval (arithptr->value, peek_left, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_CHR:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_chr (arithptr->value, peek_left,
			      peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_INSTR:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_instr (peek_left, peek_right, peek_third,
				arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_POSITION:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_position (peek_left, peek_right,
				   arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_FINDINSET:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_find_string_in_in_set (peek_left, peek_right,
					 arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_SUBSTRING:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right)
	  || (arithptr->thirdptr && DB_IS_NULL (peek_third)))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (prm_get_integer_value (PRM_ID_COMPAT_MODE) == COMPAT_MYSQL)
	{
	  DB_VALUE tmp_len, tmp_arg2, tmp_arg3;
	  int pos, len;

	  pos = db_get_int (peek_right);
	  if (pos < 0)
	    {
	      if (QSTR_IS_BIT (TP_DOMAIN_TYPE (arithptr->leftptr->domain)))
		{
		  if (db_string_bit_length (peek_left, &tmp_len) != NO_ERROR)
		    {
		      goto error;
		    }
		}
	      else
		{
		  if (db_string_char_length (peek_left, &tmp_len) != NO_ERROR)
		    {
		      goto error;
		    }
		}
	      if (DB_IS_NULL (&tmp_len))
		{
		  goto error;
		}
	      pos = pos + db_get_int (&tmp_len) + 1;
	    }

	  if (pos < 1)
	    {
	      db_make_int (&tmp_arg2, 1);
	    }
	  else
	    {
	      db_make_int (&tmp_arg2, pos);
	    }

	  if (arithptr->thirdptr)
	    {
	      len = db_get_int (peek_third);
	      if (len < 1)
		{
		  db_make_int (&tmp_arg3, 0);
		}
	      else
		{
		  db_make_int (&tmp_arg3, len);
		}
	    }
	  else
	    {
	      db_make_null (&tmp_arg3);
	    }

	  if (db_string_substring (arithptr->misc_operand, peek_left,
				   &tmp_arg2, &tmp_arg3,
				   arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      else
	{
	  if (db_string_substring (arithptr->misc_operand,
				   peek_left, peek_right, peek_third,
				   arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_OCTET_LENGTH:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  db_make_int (arithptr->value, db_get_string_size (peek_right));
	}
      break;

    case T_BIT_LENGTH:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (DB_VALUE_DOMAIN_TYPE (peek_right) == DB_TYPE_BIT
	       || DB_VALUE_DOMAIN_TYPE (peek_right) == DB_TYPE_VARBIT)
	{
	  int len = 0;

	  db_get_bit (peek_right, &len);
	  db_make_int (arithptr->value, len);
	}
      else
	{
	  /* must be a char string type */
	  db_make_int (arithptr->value, 8 * DB_GET_STRING_SIZE (peek_right));
	}
      break;

    case T_CHAR_LENGTH:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  db_make_int (arithptr->value, DB_GET_STRING_LENGTH (peek_right));
	}
      break;

    case T_LOWER:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_lower (peek_right, arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_UPPER:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_upper (peek_right, arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_HEX:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_hex (peek_right, arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_ASCII:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_ascii (peek_right, arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_CONV:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right)
	  || DB_IS_NULL (peek_third))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (db_conv (peek_left, peek_right, peek_third, arithptr->value)
	      != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_BIN:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_bigint_to_binary_string (peek_right, arithptr->value)
	       != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_MD5:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_md5 (peek_right, arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_SHA_ONE:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_sha_one (peek_right, arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_AES_ENCRYPT:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_aes_encrypt (peek_left, peek_right,
				      arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_AES_DECRYPT:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_aes_decrypt (peek_left, peek_right,
				      arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_SHA_TWO:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_sha_two (peek_left, peek_right,
				  arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_SPACE:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_space (peek_right, arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TRIM:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_trim (arithptr->misc_operand,
			       peek_right, peek_left,
			       arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_LTRIM:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_trim (LEADING,
			       peek_right, peek_left,
			       arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_RTRIM:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_trim (TRAILING,
			       peek_right, peek_left,
			       arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_FROM_UNIXTIME:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_from_unixtime (peek_left, peek_right, arithptr->value)
	       != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_LPAD:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_pad (LEADING, peek_left, peek_right, peek_third,
			      arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_RPAD:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_pad (TRAILING, peek_left, peek_right, peek_third,
			      arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_REPLACE:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_replace (peek_left, peek_right, peek_third,
				  arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TRANSLATE:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_string_translate (peek_left, peek_right, peek_third,
				    arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_ADD_MONTHS:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_add_months (peek_left, peek_right,
			      arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_LAST_DAY:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_last_day (peek_right, arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TIME_FORMAT:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_time_format (peek_left, peek_right, peek_third,
			       arithptr->value, arithptr->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_YEAR:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_date_item (peek_right, PT_YEARF,
				 arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_MONTH:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_date_item (peek_right, PT_MONTHF,
				 arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DAY:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_date_item (peek_right, PT_DAYF,
				 arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_HOUR:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_time_item (peek_right, PT_HOURF,
				 arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_MINUTE:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_time_item (peek_right, PT_MINUTEF,
				 arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_SECOND:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_time_item (peek_right, PT_SECONDF,
				 arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_QUARTER:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_date_quarter (peek_right, arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_WEEKDAY:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_date_weekday (peek_right, PT_WEEKDAY,
				    arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DAYOFWEEK:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_date_weekday (peek_right, PT_DAYOFWEEK,
				    arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DAYOFYEAR:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_date_dayofyear (peek_right, arithptr->value)
	       != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TODAYS:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_date_totaldays (peek_right, arithptr->value)
	       != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_FROMDAYS:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_get_date_from_days (peek_right, arithptr->value)
	       != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TIMETOSEC:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_convert_time_to_sec (peek_right, arithptr->value)
	       != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_SECTOTIME:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_convert_sec_to_time (peek_right, arithptr->value)
	       != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TIMESTAMP:
      if (DB_IS_NULL (peek_left)
	  || (peek_right != NULL && DB_IS_NULL (peek_right)))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (db_timestamp (peek_left, peek_right, arithptr->value) !=
	      NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_LIKE_LOWER_BOUND:
    case T_LIKE_UPPER_BOUND:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  const bool compute_lower_bound =
	    (arithptr->opcode == T_LIKE_LOWER_BOUND);

	  if (db_like_bound (peek_left, peek_right, arithptr->value,
			     compute_lower_bound) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_MAKEDATE:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (db_add_days_to_year (peek_left, peek_right, arithptr->value)
	      != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_ADDTIME:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (db_add_time (peek_left, peek_right, arithptr->value,
			   regu_var->domain) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_MAKETIME:
      if (DB_IS_NULL (peek_left)
	  || DB_IS_NULL (peek_right) || DB_IS_NULL (peek_third))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (db_convert_to_time (peek_left, peek_right, peek_third,
				  arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_WEEK:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (db_get_date_week (peek_left, peek_right, arithptr->value)
	      != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_UNIX_TIMESTAMP:
      if (arithptr->rightptr)
	{
	  if (DB_IS_NULL (peek_right))
	    {
	      PRIM_SET_NULL (arithptr->value);
	    }
	  else if (db_unix_timestamp (peek_right, arithptr->value) !=
		   NO_ERROR)
	    {
	      goto error;
	    }
	}
      else
	{
	  time_t now;

	  now = time (NULL);
	  if (now < (time_t) 0)
	    {
	      goto error;
	    }

	  db_make_int (arithptr->value, now);
	}
      break;

    case T_MONTHS_BETWEEN:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_months_between (peek_left, peek_right,
				  arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_ATAN2:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_atan2_dbval (arithptr->value, peek_left, peek_right) !=
	       NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_ATAN:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_atan_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_FORMAT:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_format (peek_left, peek_right, peek_third, arithptr->value,
			  arithptr->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DATE_FORMAT:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_date_format (peek_left, peek_right, peek_third,
			       arithptr->value, arithptr->domain) != NO_ERROR)
	{
	  goto error;
	}

      break;

    case T_STR_TO_DATE:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right)
	  || DB_IS_NULL (peek_third))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_str_to_date (peek_left, peek_right, peek_third,
			       arithptr->value, regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_ADDDATE:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	if (db_date_add_interval_days (arithptr->value, peek_left, peek_right)
	    != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DATE_ADD:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  int unit = db_get_int (peek_third);
	  if (db_date_add_interval_expr (arithptr->value, peek_left,
					 peek_right, unit) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_SUBDATE:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	if (db_date_sub_interval_days (arithptr->value, peek_left, peek_right)
	    != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DATEDIFF:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_date_diff (peek_left, peek_right,
			     arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TIMEDIFF:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_time_diff (peek_left, peek_right,
			     arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DATE_SUB:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right)
	  || DB_IS_NULL (peek_third))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  int unit = db_get_int (peek_third);

	  if (db_date_sub_interval_expr (arithptr->value, peek_left,
					 peek_right, unit) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_SYS_DATE:
      DB_MAKE_ENCODED_DATE (arithptr->value, &vd->sys_datetime.date);
      break;

    case T_SYS_TIME:
      {
	DB_TIME db_time;

	db_time = vd->sys_datetime.time / 1000;
	DB_MAKE_ENCODED_TIME (arithptr->value, &db_time);
	break;
      }

    case T_SYS_TIMESTAMP:
      {
	DB_TIMESTAMP db_timestamp;
	DB_TIME db_time;

	db_time = vd->sys_datetime.time / 1000;

	db_timestamp_encode (&db_timestamp, &vd->sys_datetime.date, &db_time);
	db_make_timestamp (arithptr->value, db_timestamp);
      }
      break;

    case T_SYS_DATETIME:
      DB_MAKE_DATETIME (arithptr->value, &vd->sys_datetime);
      break;

    case T_UTC_TIME:
      {
	DB_TIME db_time;
	DB_VALUE timezone;
	int timezone_val;

	db_time = vd->sys_datetime.time / 1000;

	/* extract the timezone part */
	if (db_sys_timezone (&timezone) != NO_ERROR)
	  {
	    goto error;
	  }
	timezone_val = DB_GET_INT (&timezone);
	db_time = db_time + timezone_val * 60 + SECONDS_OF_ONE_DAY;
	db_time = db_time % SECONDS_OF_ONE_DAY;

	DB_MAKE_ENCODED_TIME (arithptr->value, &db_time);
	break;
      }

    case T_UTC_DATE:
      {
	DB_VALUE timezone;
	DB_BIGINT timezone_milis;
	DB_DATETIME db_datetime;
	DB_DATE db_date;

	/* extract the timezone part */
	if (db_sys_timezone (&timezone) != NO_ERROR)
	  {
	    goto error;
	  }
	timezone_milis = DB_GET_INT (&timezone) * 60000;
	if (db_add_int_to_datetime (&vd->sys_datetime, timezone_milis,
				    &db_datetime) != NO_ERROR)
	  {
	    goto error;
	  }

	db_date = db_datetime.date;
	DB_MAKE_ENCODED_DATE (arithptr->value, &db_date);
	break;
      }
    case T_LOCAL_TRANSACTION_ID:
      db_make_int (arithptr->value, logtb_find_current_tranid (thread_p));
      break;

    case T_TO_CHAR:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_to_char (peek_left, peek_right, peek_third,
			   arithptr->value, arithptr->domain) != NO_ERROR)
	{
	  goto error;
	}

      break;

    case T_BLOB_TO_BIT:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_blob_to_bit (peek_left, peek_right, arithptr->value) !=
	       NO_ERROR)
	{
	  goto error;
	}

      break;

    case T_CLOB_TO_CHAR:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_clob_to_char (peek_left, peek_right, arithptr->value) !=
	       NO_ERROR)
	{
	  goto error;
	}

      break;

    case T_BIT_TO_BLOB:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (DB_VALUE_DOMAIN_TYPE (peek_left) == DB_TYPE_BIT
	       || DB_VALUE_DOMAIN_TYPE (peek_left) == DB_TYPE_VARBIT)
	{
	  if (db_bit_to_blob (peek_left, arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      else			/* (DB_VALUE_DOMAIN_TYPE (peek_left) == DB_TYPE_CHAR || DB_TYPE_VARCHAR) */
	{
	  if (db_char_to_blob (peek_left, arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}

      break;

    case T_CHAR_TO_CLOB:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_char_to_clob (peek_left, arithptr->value) != NO_ERROR)
	{
	  goto error;
	}

      break;

    case T_LOB_LENGTH:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (DB_VALUE_DOMAIN_TYPE (peek_left) == DB_TYPE_BLOB)
	{

	  if (db_blob_length (peek_left, arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      else			/* (DB_VALUE_DOMAIN_TYPE (peek_left) == DB_TYPE_BLOB) */
	{
	  if (db_clob_length (peek_left, arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}

      break;

    case T_TO_DATE:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_to_date (peek_left, peek_right, peek_third,
			   arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TO_TIME:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_to_time (peek_left, peek_right, peek_third,
			   arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TO_TIMESTAMP:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_to_timestamp (peek_left, peek_right, peek_third,
				arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TO_DATETIME:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_to_datetime (peek_left, peek_right, peek_third,
			       arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TO_NUMBER:
      if (DB_IS_NULL (peek_left))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (db_to_number (peek_left, peek_right, peek_third,
			    arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	  regu_var->domain->precision =
	    arithptr->value->domain.numeric_info.precision;
	  regu_var->domain->scale =
	    arithptr->value->domain.numeric_info.scale;
	}
      break;

    case T_CURRENT_VALUE:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  OID *serial_oid;
	  int cached_num;

	  serial_oid = DB_GET_OID (peek_left);
	  cached_num = DB_GET_INTEGER (peek_right);

	  if (xserial_get_current_value (thread_p, arithptr->value,
					 serial_oid, cached_num) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_NEXT_VALUE:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right)
	  || DB_IS_NULL (peek_third))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  OID *serial_oid;
	  int cached_num;
	  int num_alloc;

	  serial_oid = DB_GET_OID (peek_left);
	  cached_num = DB_GET_INTEGER (peek_right);
	  num_alloc = DB_GET_INTEGER (peek_third);

	  if (xserial_get_next_value (thread_p, arithptr->value, serial_oid,
				      cached_num, num_alloc, GENERATE_SERIAL,
				      false) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_CAST:
      if (REGU_VARIABLE_IS_FLAGED (regu_var, REGU_VARIABLE_APPLY_COLLATION))
	{
	  pr_clone_value (peek_right, arithptr->value);

	  if (TP_IS_CHAR_TYPE (TP_DOMAIN_TYPE (regu_var->domain)))
	    {
	      if (!DB_IS_NULL (arithptr->value))
		{
		  assert (TP_IS_CHAR_TYPE (DB_VALUE_TYPE (arithptr->value)));
		  assert (regu_var->domain->codeset
			  == DB_GET_STRING_CODESET (arithptr->value));
		}
	      db_string_put_cs_and_collation (arithptr->value,
					      regu_var->domain->codeset,
					      regu_var->domain->collation_id);
	    }
	  else
	    {
	      assert (TP_DOMAIN_TYPE (regu_var->domain) ==
		      DB_TYPE_ENUMERATION);

	      if (!DB_IS_NULL (arithptr->value))
		{
		  assert (DB_VALUE_DOMAIN_TYPE (arithptr->value)
			  == DB_TYPE_ENUMERATION);
		  assert (regu_var->domain->codeset
			  == DB_GET_ENUM_CODESET (arithptr->value));
		}
	      db_enum_put_cs_and_collation (arithptr->value,
					    regu_var->domain->codeset,
					    regu_var->domain->collation_id);

	    }
	}
      else
	{
	  dom_status = tp_value_strict_cast (peek_right, arithptr->value,
					     arithptr->domain);
	  if (dom_status != DOMAIN_COMPATIBLE)
	    {
	      (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
					      peek_right, arithptr->domain);
	      goto error;
	    }
	}
      break;

    case T_CAST_NOFAIL:
      if (REGU_VARIABLE_IS_FLAGED (regu_var, REGU_VARIABLE_APPLY_COLLATION))
	{
	  pr_clone_value (peek_right, arithptr->value);

	  if (TP_IS_CHAR_TYPE (TP_DOMAIN_TYPE (regu_var->domain)))
	    {
	      if (!DB_IS_NULL (arithptr->value))
		{
		  assert (TP_IS_CHAR_TYPE (DB_VALUE_TYPE (arithptr->value)));
		  assert (regu_var->domain->codeset
			  == DB_GET_STRING_CODESET (arithptr->value));
		}
	      db_string_put_cs_and_collation (arithptr->value,
					      regu_var->domain->codeset,
					      regu_var->domain->collation_id);
	    }
	  else
	    {
	      assert (TP_DOMAIN_TYPE (regu_var->domain) ==
		      DB_TYPE_ENUMERATION);

	      if (!DB_IS_NULL (arithptr->value))
		{
		  assert (DB_VALUE_DOMAIN_TYPE (arithptr->value)
			  == DB_TYPE_ENUMERATION);
		  assert (regu_var->domain->codeset
			  == DB_GET_ENUM_CODESET (arithptr->value));
		}
	      db_enum_put_cs_and_collation (arithptr->value,
					    regu_var->domain->codeset,
					    regu_var->domain->collation_id);

	    }
	}
      else
	{
	  TP_DOMAIN_STATUS status;

	  status =
	    tp_value_cast (peek_right, arithptr->value, arithptr->domain,
			   false);
	  if (status != NO_ERROR)
	    {
	      PRIM_SET_NULL (arithptr->value);
	    }
	}
      break;

    case T_CASE:
    case T_DECODE:
    case T_IF:
      /* fetch values */
      switch (eval_pred (thread_p, arithptr->pred, vd, obj_oid))
	{
	case V_FALSE:
	case V_UNKNOWN:	/* unknown pred result, including cases of NULL pred operands */
	  if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				vd, NULL, obj_oid, tpl,
				&peek_left) != NO_ERROR)
	    {
	      goto error;
	    }
	  break;

	case V_TRUE:
	  if (fetch_peek_dbval (thread_p, arithptr->leftptr,
				vd, NULL, obj_oid, tpl,
				&peek_left) != NO_ERROR)
	    {
	      goto error;
	    }
	  break;

	default:
	  goto error;
	}

      dom_status =
	tp_value_coerce (peek_left, arithptr->value, regu_var->domain);
      if (dom_status != DOMAIN_COMPATIBLE)
	{
	  (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
					  peek_left, regu_var->domain);
	  goto error;
	}
      break;

    case T_PREDICATE:
      /* return 0,1 or NULL accordingly */
      peek_left = &tmp_value;

      switch (eval_pred (thread_p, arithptr->pred, vd, obj_oid))
	{
	case V_UNKNOWN:
	  DB_MAKE_NULL (peek_left);
	  break;
	case V_FALSE:
	  DB_MAKE_INT (peek_left, 0);
	  break;
	case V_TRUE:
	  DB_MAKE_INT (peek_left, 1);
	  break;
	default:
	  goto error;
	}

      dom_status =
	tp_value_coerce (peek_left, arithptr->value, regu_var->domain);
      if (dom_status != DOMAIN_COMPATIBLE)
	{
	  (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
					  peek_left, regu_var->domain);
	  goto error;
	}
      break;

    case T_NVL:
    case T_IFNULL:
    case T_COALESCE:
      {
	DB_VALUE *src;
	bool need_free = false;
	TP_DOMAIN *target_domain;

	target_domain = regu_var->domain;

	if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			      vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	  {
	    goto error;
	  }

	if (DB_IS_NULL (peek_left) || target_domain == NULL)
	  {
	    if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				  vd, NULL, obj_oid, tpl,
				  &peek_right) != NO_ERROR)
	      {
		goto error;
	      }
	  }

	if (target_domain == NULL)
	  {
	    TP_DOMAIN *arg1, *arg2, tmp_arg1, tmp_arg2;

	    arg1 = tp_domain_resolve_value (peek_left, &tmp_arg1);
	    arg2 = tp_domain_resolve_value (peek_right, &tmp_arg2);

	    target_domain = tp_infer_common_domain (arg1, arg2, &need_free);
	  }

	src = DB_IS_NULL (peek_left) ? peek_right : peek_left;
	dom_status =
	  tp_value_cast (src, arithptr->value, target_domain, false);
	if (dom_status != DOMAIN_COMPATIBLE)
	  {
	    (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE, src,
					    target_domain);
	    if (need_free)
	      {
		tp_domain_free (target_domain);
	      }
	    goto error;
	  }

	if (need_free)
	  {
	    tp_domain_free (target_domain);
	  }
      }
      break;
    case T_NVL2:
      {
	DB_VALUE *src;
	TP_DOMAIN *target_domain;
	bool need_free = false;

	target_domain = regu_var->domain;

	if (fetch_peek_dbval (thread_p, arithptr->leftptr,
			      vd, NULL, obj_oid, tpl, &peek_left) != NO_ERROR)
	  {
	    goto error;
	  }

	if (target_domain == NULL)
	  {
	    TP_DOMAIN *arg1, *arg2, *arg3, tmp_arg1, tmp_arg2, tmp_arg3;

	    if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				  vd, NULL, obj_oid, tpl,
				  &peek_right) != NO_ERROR)
	      {
		goto error;
	      }

	    if (fetch_peek_dbval (thread_p, arithptr->thirdptr,
				  vd, NULL, obj_oid, tpl,
				  &peek_third) != NO_ERROR)
	      {
		goto error;
	      }

	    arg1 = tp_domain_resolve_value (peek_left, &tmp_arg1);
	    arg2 = tp_domain_resolve_value (peek_right, &tmp_arg2);

	    target_domain = tp_infer_common_domain (arg1, arg2, &need_free);

	    arg3 = NULL;
	    if (peek_third)
	      {
		bool need_tmp_domain_free;
		TP_DOMAIN *tmp_domain;

		arg3 = tp_domain_resolve_value (peek_third, &tmp_arg3);
		tmp_domain =
		  tp_infer_common_domain (target_domain, arg3,
					  &need_tmp_domain_free);

		if (need_free)
		  {
		    tp_domain_free (target_domain);
		  }
		target_domain = tmp_domain;
		need_free = need_tmp_domain_free;
	      }
	  }

	if (DB_IS_NULL (peek_left))
	  {
	    if (peek_third == NULL)
	      {
		if (fetch_peek_dbval (thread_p, arithptr->thirdptr,
				      vd, NULL, obj_oid, tpl,
				      &peek_third) != NO_ERROR)
		  {
		    if (need_free)
		      {
			tp_domain_free (target_domain);
		      }

		    goto error;
		  }
	      }
	    src = peek_third;
	  }
	else
	  {
	    if (peek_right == NULL)
	      {
		if (fetch_peek_dbval (thread_p, arithptr->rightptr,
				      vd, NULL, obj_oid, tpl,
				      &peek_right) != NO_ERROR)
		  {
		    if (need_free)
		      {
			tp_domain_free (target_domain);
		      }
		    goto error;
		  }
	      }

	    src = peek_right;
	  }

	dom_status =
	  tp_value_cast (src, arithptr->value, target_domain, false);
	if (dom_status != DOMAIN_COMPATIBLE)
	  {
	    (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE, src,
					    target_domain);

	    if (need_free)
	      {
		tp_domain_free (target_domain);
	      }
	    goto error;
	  }

	if (need_free)
	  {
	    tp_domain_free (target_domain);
	  }
      }
      break;

    case T_ISNULL:
      if (DB_IS_NULL (peek_right))
	{
	  DB_MAKE_INTEGER (arithptr->value, 1);
	}
      else
	{
	  DB_MAKE_INTEGER (arithptr->value, 0);
	}
      break;

    case T_CONCAT:
      if (arithptr->rightptr != NULL)
	{
	  if (qdata_strcat_dbval (peek_left, peek_right,
				  arithptr->value,
				  regu_var->domain) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      else
	{
	  dom_status =
	    tp_value_coerce (peek_left, arithptr->value, regu_var->domain);
	  if (dom_status != DOMAIN_COMPATIBLE)
	    {
	      (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
					      peek_left, regu_var->domain);
	      goto error;
	    }
	}
      break;

    case T_CONCAT_WS:
      if (DB_IS_NULL (peek_third))
	{
	  PRIM_SET_NULL (arithptr->value);
	  break;
	}
      if (arithptr->rightptr != NULL)
	{
	  if (DB_IS_NULL (peek_left) && DB_IS_NULL (peek_right))
	    {
	      PRIM_SET_NULL (arithptr->value);
	    }
	  else if (DB_IS_NULL (peek_left))
	    {
	      dom_status =
		tp_value_coerce (peek_right, arithptr->value,
				 regu_var->domain);
	      if (dom_status != DOMAIN_COMPATIBLE)
		{
		  (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
						  peek_right,
						  regu_var->domain);
		  goto error;
		}
	    }
	  else if (DB_IS_NULL (peek_right))
	    {
	      dom_status =
		tp_value_coerce (peek_left, arithptr->value,
				 regu_var->domain);
	      if (dom_status != DOMAIN_COMPATIBLE)
		{
		  (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
						  peek_left,
						  regu_var->domain);
		  goto error;
		}
	    }
	  else
	    {
	      DB_VALUE tmp_val;

	      DB_MAKE_NULL (&tmp_val);
	      if (qdata_strcat_dbval (peek_left, peek_third,
				      &tmp_val, regu_var->domain) != NO_ERROR)
		{
		  goto error;
		}
	      if (qdata_strcat_dbval (&tmp_val, peek_right,
				      arithptr->value,
				      regu_var->domain) != NO_ERROR)
		{
		  (void) pr_clear_value (&tmp_val);
		  goto error;
		}
	      (void) pr_clear_value (&tmp_val);
	    }
	}
      else
	{
	  if (DB_IS_NULL (peek_left))
	    {
	      PRIM_SET_NULL (arithptr->value);
	    }
	  else
	    {
	      dom_status =
		tp_value_coerce (peek_left, arithptr->value,
				 regu_var->domain);
	      if (dom_status != DOMAIN_COMPATIBLE)
		{
		  (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
						  peek_left,
						  regu_var->domain);
		  goto error;
		}
	    }
	}
      break;

    case T_FIELD:
      if (DB_IS_NULL (peek_third))
	{
	  db_make_int (arithptr->value, 0);
	  break;
	}
      if (REGU_VARIABLE_IS_FLAGED (regu_var, REGU_VARIABLE_FIELD_COMPARE))
	{
	  if (tp_value_compare (peek_third, peek_left, 1, 0) == DB_EQ)
	    {
	      db_make_int (arithptr->value, 1);
	    }
	  else if (tp_value_compare (peek_third, peek_right, 1, 0) == DB_EQ)
	    {
	      db_make_int (arithptr->value, 2);
	    }
	  else
	    {
	      if (REGU_VARIABLE_IS_FLAGED
		  (regu_var, REGU_VARIABLE_FIELD_NESTED))
		{
		  /* we have a T_FIELD parent, return level */
		  db_make_int (arithptr->value, -3);
		}
	      else
		{
		  /* no parent and no match */
		  db_make_int (arithptr->value, 0);
		}
	    }
	}
      else
	{
	  int i = db_get_int (peek_left);
	  if (i > 0)
	    {
	      db_make_int (arithptr->value, i);
	    }
	  else
	    {
	      if (tp_value_compare (peek_third, peek_right, 1, 0) == DB_EQ)
		{
		  /* match */
		  db_make_int (arithptr->value, -i);
		}
	      else
		{
		  if (REGU_VARIABLE_IS_FLAGED
		      (regu_var, REGU_VARIABLE_FIELD_NESTED))
		    {
		      /* we have a T_FIELD parent, return level */
		      db_make_int (arithptr->value, i - 1);
		    }
		  else
		    {
		      /* no parent and no match */
		      db_make_int (arithptr->value, 0);
		    }
		}
	    }
	}
      break;

    case T_REPEAT:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (db_string_repeat (peek_left, peek_right,
				arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_LEFT:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  DB_VALUE tmp_val, tmp_val2;

	  dom_status =
	    tp_value_coerce (peek_right, &tmp_val2, &tp_Integer_domain);
	  if (dom_status != DOMAIN_COMPATIBLE)
	    {
	      (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
					      peek_right, &tp_Integer_domain);
	      goto error;
	    }

	  db_make_int (&tmp_val, 0);
	  if (db_string_substring (SUBSTRING,
				   peek_left, &tmp_val, &tmp_val2,
				   arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_RIGHT:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  DB_VALUE tmp_val, tmp_val2;

	  if (QSTR_IS_BIT (TP_DOMAIN_TYPE (arithptr->leftptr->domain)))
	    {
	      if (db_string_bit_length (peek_left, &tmp_val) != NO_ERROR)
		{
		  goto error;
		}
	    }
	  else
	    {
	      if (db_string_char_length (peek_left, &tmp_val) != NO_ERROR)
		{
		  goto error;
		}
	    }
	  if (DB_IS_NULL (&tmp_val))
	    {
	      PRIM_SET_NULL (arithptr->value);
	      break;
	    }

	  dom_status =
	    tp_value_coerce (peek_right, &tmp_val2, &tp_Integer_domain);
	  if (dom_status != DOMAIN_COMPATIBLE)
	    {
	      (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
					      peek_right, &tp_Integer_domain);
	      goto error;
	    }
	  /* If len, defined as second argument, is negative value,
	   * RIGHT function returns the entire string.
	   * It's same behavior with LEFT and SUBSTRING.
	   */
	  if (db_get_int (&tmp_val2) < 0)
	    {
	      db_make_int (&tmp_val, 0);
	    }
	  else
	    {
	      db_make_int (&tmp_val,
			   db_get_int (&tmp_val) - db_get_int (&tmp_val2) +
			   1);
	    }
	  if (db_string_substring (SUBSTRING,
				   peek_left, &tmp_val, &tmp_val2,
				   arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_LOCATE:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right)
	  || (arithptr->thirdptr && DB_IS_NULL (peek_third)))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (!arithptr->thirdptr)
	    {
	      if (db_string_position (peek_left, peek_right,
				      arithptr->value) != NO_ERROR)
		{
		  goto error;
		}
	    }
	  else
	    {
	      DB_VALUE tmp_len, tmp_val, tmp_arg3;
	      int tmp;

	      DB_MAKE_NULL (&tmp_val);

	      tmp = db_get_int (peek_third);
	      if (tmp < 1)
		{
		  db_make_int (&tmp_arg3, 1);
		}
	      else
		{
		  db_make_int (&tmp_arg3, tmp);
		}

	      if (db_string_char_length (peek_right, &tmp_len) != NO_ERROR)
		{
		  goto error;
		}
	      if (DB_IS_NULL (&tmp_len))
		{
		  goto error;
		}

	      db_make_int (&tmp_len,
			   db_get_int (&tmp_len) -
			   db_get_int (&tmp_arg3) + 1);

	      if (db_string_substring (SUBSTRING, peek_right, &tmp_arg3,
				       &tmp_len, &tmp_val) != NO_ERROR)
		{
		  goto error;
		}

	      if (db_string_position (peek_left, &tmp_val,
				      arithptr->value) != NO_ERROR)
		{
		  (void) pr_clear_value (&tmp_val);
		  goto error;
		}
	      if (db_get_int (arithptr->value) > 0)
		{
		  db_make_int (arithptr->value,
			       db_get_int (arithptr->value) +
			       db_get_int (&tmp_arg3) - 1);
		}

	      (void) pr_clear_value (&tmp_val);
	    }
	}
      break;

    case T_SUBSTRING_INDEX:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right)
	  || DB_IS_NULL (peek_third))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (db_string_substring_index (peek_left, peek_right, peek_third,
					 arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_MID:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right)
	  || DB_IS_NULL (peek_third))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  DB_VALUE tmp_len, tmp_arg2, tmp_arg3;
	  int pos, len;

	  pos = db_get_int (peek_right);
	  len = db_get_int (peek_third);

	  if (pos < 0)
	    {
	      if (QSTR_IS_BIT (TP_DOMAIN_TYPE (arithptr->leftptr->domain)))
		{
		  if (db_string_bit_length (peek_left, &tmp_len) != NO_ERROR)
		    {
		      goto error;
		    }
		}
	      else
		{
		  if (db_string_char_length (peek_left, &tmp_len) != NO_ERROR)
		    {
		      goto error;
		    }
		}
	      if (DB_IS_NULL (&tmp_len))
		{
		  goto error;
		}
	      pos = pos + db_get_int (&tmp_len) + 1;
	    }

	  if (pos < 1)
	    {
	      db_make_int (&tmp_arg2, 1);
	    }
	  else
	    {
	      db_make_int (&tmp_arg2, pos);
	    }

	  if (len < 1)
	    {
	      db_make_int (&tmp_arg3, 0);
	    }
	  else
	    {
	      db_make_int (&tmp_arg3, len);
	    }

	  if (db_string_substring (SUBSTRING, peek_left, &tmp_arg2,
				   &tmp_arg3, arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_STRCMP:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  int cmp;

	  if (db_string_compare (peek_left, peek_right,
				 arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	  cmp = db_get_int (arithptr->value);
	  if (cmp < 0)
	    {
	      cmp = -1;
	    }
	  else if (cmp > 0)
	    {
	      cmp = 1;
	    }
	  db_make_int (arithptr->value, cmp);
	}
      break;

    case T_REVERSE:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (db_string_reverse (peek_right, arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_NULLIF:		/* when a = b then null else a end */
      {
	TP_DOMAIN *target_domain;
	bool need_free = false;

	target_domain = regu_var->domain;
	if (target_domain == NULL)
	  {
	    TP_DOMAIN *arg1, *arg2, tmp_arg1, tmp_arg2;

	    arg1 = tp_domain_resolve_value (peek_left, &tmp_arg1);
	    arg2 = tp_domain_resolve_value (peek_right, &tmp_arg2);

	    target_domain = tp_infer_common_domain (arg1, arg2, &need_free);
	  }

	if (tp_value_compare (peek_left, peek_right, 1, 0) == DB_EQ)
	  {
	    PRIM_SET_NULL (arithptr->value);
	  }
	else
	  {
	    dom_status =
	      tp_value_cast (peek_left, arithptr->value, target_domain,
			     false);
	    if (dom_status != DOMAIN_COMPATIBLE)
	      {
		(void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
						peek_left, target_domain);
		if (need_free)
		  {
		    tp_domain_free (target_domain);
		  }
		goto error;
	      }
	  }

	if (need_free)
	  {
	    tp_domain_free (target_domain);
	  }
      }
      break;

    case T_EXTRACT:
      if (qdata_extract_dbval (arithptr->misc_operand, peek_right,
			       arithptr->value, regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_LEAST:
      {
	int cmp_result;
	TP_DOMAIN *target_domain;
	bool need_free = false;

	cmp_result = tp_value_compare (peek_left, peek_right, 1, 0);
	if (cmp_result == DB_EQ || cmp_result == DB_LT)
	  {
	    pr_clone_value (peek_left, arithptr->value);
	  }
	else if (cmp_result == DB_GT)
	  {
	    pr_clone_value (peek_right, arithptr->value);
	  }
	else
	  {
	    if (DB_IS_NULL (peek_right) || DB_IS_NULL (peek_left))
	      {
		pr_clear_value (arithptr->value);
		PRIM_SET_NULL (arithptr->value);
	      }
	    else
	      {
		assert_release (false);
	      }
	    break;
	  }

	target_domain = regu_var->domain;
	if (target_domain == NULL)
	  {
	    TP_DOMAIN *arg1, *arg2, tmp_arg1, tmp_arg2;

	    arg1 = tp_domain_resolve_value (peek_left, &tmp_arg1);
	    arg2 = tp_domain_resolve_value (peek_right, &tmp_arg2);

	    target_domain = tp_infer_common_domain (arg1, arg2, &need_free);
	  }

	dom_status =
	  tp_value_cast (arithptr->value, arithptr->value, target_domain,
			 false);
	if (dom_status != DOMAIN_COMPATIBLE)
	  {
	    (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
					    arithptr->value, target_domain);
	    if (need_free)
	      {
		tp_domain_free (target_domain);
	      }
	    goto error;
	  }

	if (need_free)
	  {
	    tp_domain_free (target_domain);
	  }
	break;
      }

    case T_GREATEST:
      {
	int cmp_result;
	TP_DOMAIN *target_domain;
	bool need_free = false;

	cmp_result = tp_value_compare (peek_left, peek_right, 1, 0);
	if (cmp_result == DB_EQ || cmp_result == DB_GT)
	  {
	    pr_clone_value (peek_left, arithptr->value);
	  }
	else if (cmp_result == DB_LT)
	  {
	    pr_clone_value (peek_right, arithptr->value);
	  }
	else
	  {
	    if (DB_IS_NULL (peek_right) || DB_IS_NULL (peek_left))
	      {
		pr_clear_value (arithptr->value);
		PRIM_SET_NULL (arithptr->value);
	      }
	    else
	      {
		assert_release (false);
	      }
	    break;
	  }

	target_domain = regu_var->domain;
	if (target_domain == NULL)
	  {
	    TP_DOMAIN *arg1, *arg2, tmp_arg1, tmp_arg2;

	    arg1 = tp_domain_resolve_value (peek_left, &tmp_arg1);
	    arg2 = tp_domain_resolve_value (peek_right, &tmp_arg2);

	    target_domain = tp_infer_common_domain (arg1, arg2, &need_free);
	  }

	dom_status =
	  tp_value_cast (arithptr->value, arithptr->value, target_domain,
			 false);
	if (dom_status != DOMAIN_COMPATIBLE)
	  {
	    (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
					    arithptr->value, target_domain);
	    if (need_free)
	      {
		tp_domain_free (target_domain);
	      }
	    goto error;
	  }

	if (need_free)
	  {
	    tp_domain_free (target_domain);
	  }
	break;
      }

    case T_SYS_CONNECT_BY_PATH:
      {
	if (!qdata_evaluate_sys_connect_by_path (thread_p,
						 (void *) arithptr->
						 thirdptr->xasl,
						 arithptr->leftptr,
						 peek_right, arithptr->value,
						 vd))
	  {
	    goto error;
	  }

	dom_status =
	  tp_value_coerce (arithptr->value, arithptr->value,
			   regu_var->domain);
	if (dom_status != DOMAIN_COMPATIBLE)
	  {
	    (void) tp_domain_status_er_set (dom_status, ARG_FILE_LINE,
					    arithptr->value,
					    regu_var->domain);
	    goto error;
	  }
	break;
      }

    case T_STRCAT:
      if (qdata_strcat_dbval (peek_left, peek_right,
			      arithptr->value, regu_var->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_PI:
      db_make_double (arithptr->value, PI);
      break;

    case T_ROW_COUNT:
      {
	int row_count = -1;
	if (session_get_row_count (thread_p, &row_count) != NO_ERROR)
	  {
	    goto error;
	  }
	DB_MAKE_INTEGER (arithptr->value, row_count);
      }
      break;

    case T_LAST_INSERT_ID:
      if (session_get_last_insert_id (thread_p, arithptr->value, true) !=
	  NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_EVALUATE_VARIABLE:
      if (session_get_variable (thread_p, peek_right, arithptr->value)
	  != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_DEFINE_VARIABLE:
      if (session_define_variable (thread_p, peek_left, peek_right,
				   arithptr->value) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_RAND:
    case T_RANDOM:
      if (DB_IS_NULL (peek_right))
	{
	  /* When random functions are called without a seed, peek_right is null.
	   * In this case, rand() or drand() uses a random value stored on value descriptor
	   * to generate the same number during executing one SELECT statement.
	   * But, each random() or drandom() picks up a seed value to generate different
	   * numbers at every call.
	   */
	  if (arithptr->opcode == T_RAND)
	    {
	      db_make_int (arithptr->value, (int) vd->lrand);
	    }
	  else
	    {
	      long int r;
	      struct drand48_data *rand_buf_p;

	      rand_buf_p = qmgr_get_rand_buf (thread_p);
	      lrand48_r (rand_buf_p, &r);
	      db_make_int (arithptr->value, r);
	    }
	}
      else
	{
	  /* There are two types of seed:
	   *  1) given by user (rightptr's type is TYPE_DBVAL)
	   *   e.g, select rand(1) from table;
	   *  2) fetched from tuple (rightptr's type is TYPE_CONSTANT)
	   *   e.g, select rand(i) from table;
	   *
	   * Regarding the former case, rand(1) will generate a sequence of pseudo-random
	   * values up to the number of rows. However, on the latter case, rand(i) generates
	   * values depending on the column i's value. If, for example, there are three
	   * tuples which include column i of 1 in a table, results of the above statements
	   * are as follows.
	   *
	   *       rand(1)             rand(i)
	   * =============       =============
	   *      89400484            89400484
	   *     976015093            89400484
	   *    1792756325            89400484
	   */
	  if (arithptr->rightptr->type == TYPE_CONSTANT)
	    {
	      struct drand48_data buf;
	      long int r;

	      srand48_r ((long) db_get_int (peek_right), &buf);
	      lrand48_r (&buf, &r);
	      db_make_int (arithptr->value, r);
	    }
	  else
	    {
	      long int r;

	      if (arithptr->rand_seed == NULL)
		{
		  arithptr->rand_seed =
		    (struct drand48_data *)
		    malloc (sizeof (struct drand48_data));

		  if (arithptr->rand_seed == NULL)
		    {
		      goto error;
		    }

		  srand48_r ((long) db_get_int (peek_right),
			     arithptr->rand_seed);
		}

	      lrand48_r (arithptr->rand_seed, &r);
	      db_make_int (arithptr->value, r);
	    }
	}
      break;

    case T_DRAND:
    case T_DRANDOM:
      if (DB_IS_NULL (peek_right))
	{
	  if (arithptr->opcode == T_DRAND)
	    {
	      db_make_double (arithptr->value, (double) vd->drand);
	    }
	  else
	    {
	      double r;
	      struct drand48_data *rand_buf_p;

	      rand_buf_p = qmgr_get_rand_buf (thread_p);
	      drand48_r (rand_buf_p, &r);
	      db_make_double (arithptr->value, r);
	    }
	}
      else
	{
	  if (arithptr->rightptr->type == TYPE_CONSTANT)
	    {
	      struct drand48_data buf;
	      double r;

	      srand48_r ((long) db_get_int (peek_right), &buf);
	      drand48_r (&buf, &r);
	      db_make_double (arithptr->value, r);
	    }
	  else
	    {
	      double r;

	      if (arithptr->rand_seed == NULL)
		{
		  arithptr->rand_seed =
		    (struct drand48_data *)
		    malloc (sizeof (struct drand48_data));

		  if (arithptr->rand_seed == NULL)
		    {
		      goto error;
		    }

		  srand48_r ((long) db_get_int (peek_right),
			     arithptr->rand_seed);
		}

	      drand48_r (arithptr->rand_seed, &r);
	      db_make_double (arithptr->value, r);
	    }
	}
      break;

    case T_LIST_DBS:
      if (qdata_list_dbs (thread_p, arithptr->value, arithptr->domain)
	  != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_TYPEOF:
      if (db_typeof_dbval (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_INDEX_CARDINALITY:
      if (DB_IS_NULL (peek_left) || DB_IS_NULL (peek_right)
	  || DB_IS_NULL (peek_third))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else
	{
	  if (qdata_get_cardinality
	      (thread_p, peek_left, peek_right, peek_third,
	       arithptr->value) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case T_EXEC_STATS:
      {
	if (session_get_exec_stats_and_clear (thread_p, peek_right,
					      arithptr->value) != NO_ERROR)
	  {
	    goto error;
	  }
      }
      break;

    case T_TO_ENUMERATION_VALUE:
      if (db_value_to_enumeration_value (peek_right, arithptr->value,
					 arithptr->domain) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_INET_ATON:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_inet_aton (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_INET_NTOA:
      if (DB_IS_NULL (peek_right))
	{
	  PRIM_SET_NULL (arithptr->value);
	}
      else if (db_inet_ntoa (arithptr->value, peek_right) != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_CHARSET:
    case T_COLLATION:
      if (db_get_cs_coll_info (arithptr->value, peek_right,
			       (arithptr->opcode == T_COLLATION) ? 1 : 0)
	  != NO_ERROR)
	{
	  goto error;
	}
      break;

    case T_WIDTH_BUCKET:
      if (db_width_bucket (arithptr->value, peek_left,
			   peek_right, peek_third, peek_fourth) != NO_ERROR)
	{
	  goto error;
	}
      break;
    }

  *peek_dbval = arithptr->value;

  if (original_domain != NULL
      && TP_DOMAIN_TYPE (original_domain) == DB_TYPE_VARIABLE)
    {
      TP_DOMAIN *resolved_dom =
	tp_domain_resolve_value (arithptr->value, NULL);

      /*keep DB_TYPE_VARIABLE if resolved domain is NULL */
      if (TP_DOMAIN_TYPE (resolved_dom) != DB_TYPE_NULL)
	{
	  regu_var->domain = arithptr->domain = resolved_dom;
	}
      else
	{
	  regu_var->domain = arithptr->domain = original_domain;
	}
    }

  return NO_ERROR;

error:

  return ER_FAILED;
}

/*
 * fetch_peek_dbval () - returns a POINTER to an existing db_value
 *   return: NO_ERROR or ER_code
 *   regu_var(in/out): Regulator Variable
 *   vd(in): Value Descriptor
 *   cls_oid(in): Class Identifier
 *   obj_oid(in): Object Identifier
 *   tpl(in): Tuple
 *   peek_dbval(out): Set to the value ref resulting from the fetch operation
 *
 */
int
fetch_peek_dbval (THREAD_ENTRY * thread_p, REGU_VARIABLE * regu_var,
		  VAL_DESCR * vd, OID * class_oid, OID * obj_oid,
		  QFILE_TUPLE tpl, DB_VALUE ** peek_dbval)
{
  int length;
  PR_TYPE *pr_type;
  OR_BUF buf;
  QFILE_TUPLE_VALUE_FLAG flag;
  char *ptr;
  REGU_VARIABLE *head_regu = NULL, *regu = NULL;
  int error = NO_ERROR;
  REGU_VALUE_LIST *reguval_list = NULL;
  DB_TYPE head_type, cur_type;

  switch (regu_var->type)
    {
    case TYPE_ATTR_ID:		/* fetch object attribute value */
    case TYPE_SHARED_ATTR_ID:
    case TYPE_CLASS_ATTR_ID:
      *peek_dbval = regu_var->value.attr_descr.cache_dbvalp;
      if (*peek_dbval != NULL)
	{
	  /* we have a cached pointer already */
	  break;
	}
      else
	{
	  *peek_dbval =
	    heap_attrinfo_access (regu_var->value.attr_descr.id,
				  regu_var->value.attr_descr.cache_attrinfo);
	  if (*peek_dbval == NULL)
	    {
	      goto error;
	    }
	}
      regu_var->value.attr_descr.cache_dbvalp = *peek_dbval;	/* cache */
      break;

    case TYPE_OID:		/* fetch object identifier value */
      *peek_dbval = &regu_var->value.dbval;
      DB_MAKE_OID (*peek_dbval, obj_oid);
      break;

    case TYPE_CLASSOID:	/* fetch class identifier value */
      *peek_dbval = &regu_var->value.dbval;
      DB_MAKE_OID (*peek_dbval, class_oid);
      break;

    case TYPE_POSITION:	/* fetch list file tuple value */
      pr_clear_value (regu_var->vfetch_to);

      *peek_dbval = regu_var->vfetch_to;

      flag =
	(QFILE_TUPLE_VALUE_FLAG) qfile_locate_tuple_value (tpl,
							   regu_var->
							   value.pos_descr.
							   pos_no, &ptr,
							   &length);
      if (flag == V_BOUND)
	{
	  pr_type = regu_var->value.pos_descr.dom->type;
	  if (pr_type == NULL)
	    {
	      goto error;
	    }

	  OR_BUF_INIT (buf, ptr, length);

	  if ((*(pr_type->data_readval)) (&buf, *peek_dbval,
					  regu_var->value.pos_descr.dom, -1,
					  false /* Don't copy */ ,
					  NULL, 0) != NO_ERROR)
	    {
	      goto error;
	    }
	}
      break;

    case TYPE_POS_VALUE:	/* fetch positional value */
#if defined(QP_DEBUG)
      if (regu_var->value.val_pos < 0 ||
	  regu_var->value.val_pos > (vd->dbval_cnt - 1))
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		  ER_QPROC_INVALID_VALLIST_INDEX, 1, regu_var->value.val_pos);
	  goto error;
	}
#endif

      *peek_dbval = (DB_VALUE *) vd->dbval_ptr + regu_var->value.val_pos;
      break;

    case TYPE_CONSTANT:	/* fetch constant value */
      /* execute linked query */
      EXECUTE_REGU_VARIABLE_XASL (thread_p, regu_var, vd);
      if (CHECK_REGU_VARIABLE_XASL_STATUS (regu_var) != XASL_SUCCESS)
	{
	  goto error;
	}
      *peek_dbval = regu_var->value.dbvalptr;
      break;

    case TYPE_ORDERBY_NUM:
      *peek_dbval = regu_var->value.dbvalptr;
      break;

    case TYPE_DBVAL:		/* fetch db_value */
      *peek_dbval = &regu_var->value.dbval;
      break;

    case TYPE_REGUVAL_LIST:
      reguval_list = regu_var->value.reguval_list;
      assert (reguval_list != NULL && reguval_list->current_value != NULL);

      regu = reguval_list->current_value->value;
      assert (regu != NULL);

      if (regu->type != TYPE_DBVAL && regu->type != TYPE_INARITH
	  && regu->type != TYPE_POS_VALUE)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_QPROC_INVALID_XASLNODE,
		  0);
	  goto error;
	}

      error = fetch_peek_dbval (thread_p, regu, vd,
				class_oid, obj_oid, tpl, peek_dbval);
      if (error != NO_ERROR)
	{
	  goto error;
	}
      break;

    case TYPE_INARITH:		/* compute and fetch arithmetic expr. value */
    case TYPE_OUTARITH:
      error = fetch_peek_arith (thread_p, regu_var, vd, obj_oid, tpl,
				peek_dbval);
      if (error != NO_ERROR)
	{
	  goto error;
	}
      break;

    case TYPE_AGGREGATE:	/* fetch aggregation function value */
      /* The result value of the aggregate node MUST already have been evaluated */
      *peek_dbval = regu_var->value.aggptr->value;
      break;

    case TYPE_FUNC:		/* fetch function value */
      if (qdata_evaluate_function (thread_p, regu_var, vd, obj_oid, tpl) !=
	  NO_ERROR)
	{
	  goto error;
	}
      *peek_dbval = regu_var->value.funcp->value;
      break;

    default:
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_QPROC_INVALID_XASLNODE, 0);
      goto error;
    }

  if (REGU_VARIABLE_IS_FLAGED (regu_var, REGU_VARIABLE_APPLY_COLLATION))
    {
      if (TP_IS_CHAR_TYPE (TP_DOMAIN_TYPE (regu_var->domain)))
	{
	  if (!DB_IS_NULL (*peek_dbval))
	    {
	      assert (TP_IS_CHAR_TYPE (DB_VALUE_TYPE (*peek_dbval)));
	      assert (regu_var->domain->codeset
		      == DB_GET_STRING_CODESET (*peek_dbval));
	    }
	  db_string_put_cs_and_collation (*peek_dbval,
					  regu_var->domain->codeset,
					  regu_var->domain->collation_id);
	}
      else
	{
	  assert (TP_DOMAIN_TYPE (regu_var->domain) == DB_TYPE_ENUMERATION);

	  if (!DB_IS_NULL (*peek_dbval))
	    {
	      assert (DB_VALUE_DOMAIN_TYPE (*peek_dbval)
		      == DB_TYPE_ENUMERATION);
	      assert (regu_var->domain->codeset
		      == DB_GET_ENUM_CODESET (*peek_dbval));
	    }
	  db_enum_put_cs_and_collation (*peek_dbval,
					regu_var->domain->codeset,
					regu_var->domain->collation_id);

	}
    }

  if (*peek_dbval != NULL && !DB_IS_NULL (*peek_dbval))
    {
      if (TP_DOMAIN_TYPE (regu_var->domain) == DB_TYPE_VARIABLE)
	{
	  regu_var->domain = tp_domain_resolve_value (*peek_dbval, NULL);
	}

      /* for REGUVAL_LIST
       * compare type with the corresponding column of first row
       * if not compatible, raise an error
       * This is the same behavior as "union"
       */
      if (regu_var->type == TYPE_REGUVAL_LIST)
	{
	  head_regu = reguval_list->regu_list->value;
	  regu = reguval_list->current_value->value;

	  if (regu->domain == NULL
	      || TP_DOMAIN_TYPE (regu->domain) == DB_TYPE_VARIABLE)
	    {
	      regu->domain = tp_domain_resolve_value (*peek_dbval, NULL);
	    }
	  head_type = TP_DOMAIN_TYPE (head_regu->domain);
	  cur_type = TP_DOMAIN_TYPE (regu->domain);

	  /* compare the type */
	  if (head_type != DB_TYPE_NULL
	      && cur_type != DB_TYPE_NULL
	      && head_regu->domain != regu->domain
	      && (head_type != cur_type
		  || !pr_is_string_type (head_type)
		  || !pr_is_variable_type (head_type)))
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		      ER_QPROC_INCOMPATIBLE_TYPES, 0);
	      goto error;
	    }
	}
    }

  return NO_ERROR;

error:

  return ER_FAILED;
}

/*
 * fetch_peek_dbval_pos () -
 *   return: NO_ERROR or ER_code
 *   regu_var(in/out): Regulator Variable
 *   tpl(in): Tuple
 *   pos(in):
 *   peek_dbval(out): Set to the value ref resulting from the fetch operation
 *   next_tpl(out): Set to the next tuple ref
 */
static int
fetch_peek_dbval_pos (REGU_VARIABLE * regu_var, QFILE_TUPLE tpl,
		      int pos, DB_VALUE ** peek_dbval, QFILE_TUPLE * next_tpl)
{
  int length;
  PR_TYPE *pr_type;
  OR_BUF buf;
  char *ptr;
  QFILE_TUPLE_VALUE_POSITION *pos_descr;

  /* assume regu_var->type == TYPE_POSITION */
  pos_descr = &regu_var->value.pos_descr;
  pr_clear_value (regu_var->vfetch_to);
  *peek_dbval = regu_var->vfetch_to;

  /* locate value position in the tuple */
  if (qfile_locate_tuple_value_r (tpl, pos, &ptr, &length) == V_BOUND)
    {
      pr_type = pos_descr->dom->type;
      if (pr_type == NULL)
	{
	  return ER_FAILED;
	}

      OR_BUF_INIT (buf, ptr, length);
      /* read value from the tuple */
      if ((*(pr_type->data_readval)) (&buf, *peek_dbval, pos_descr->dom,
				      -1, false /* Don't copy */ ,
				      NULL, 0) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }

  /* next position pointer */
  *next_tpl = ptr + length;

  return NO_ERROR;
}

/*
 * fetch_peek_min_max_value_of_width_bucket_func () -
 *   return: NO_ERROR or ER_code
 *   regu_var(in): Regulator Variable of an ARITH node.
 *   vd(in): Value Descriptor
 *   obj_oid(in): Object Identifier
 *   tpl(in): Tuple
 *   min(out): the lower bound of width_bucket
 *   max(out): the upper bound of width_bucket
 */
static int
fetch_peek_min_max_value_of_width_bucket_func (THREAD_ENTRY * thread_p,
					       REGU_VARIABLE * regu_var,
					       VAL_DESCR * vd,
					       OID * obj_oid,
					       QFILE_TUPLE tpl,
					       DB_VALUE ** min,
					       DB_VALUE ** max)
{
  int er_status = NO_ERROR;
  PRED_EXPR *pred_expr;
  PRED *pred;
  EVAL_TERM *eval_term1, *eval_term2;

  assert (min != NULL && max != NULL);

  if (regu_var == NULL || regu_var->type != TYPE_INARITH)
    {
      er_status = ER_QPROC_INVALID_XASLNODE;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, er_status, 0);

      goto error;
    }

  pred_expr = regu_var->value.arithptr->pred;
  if (pred_expr == NULL || pred_expr->type != T_PRED)
    {
      er_status = ER_QPROC_INVALID_XASLNODE;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, er_status, 0);

      goto error;
    }

  pred = &pred_expr->pe.pred;
  if (pred->lhs == NULL || pred->lhs->type != T_EVAL_TERM)
    {
      er_status = ER_QPROC_INVALID_XASLNODE;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, er_status, 0);

      goto error;
    }

  eval_term1 = &pred->lhs->pe.eval_term;
  if (eval_term1->et_type != T_COMP_EVAL_TERM)
    {
      er_status = ER_QPROC_INVALID_XASLNODE;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, er_status, 0);

      goto error;
    }

  /* lower bound, error info is already set in fetch_peek_dbval */
  er_status = fetch_peek_dbval (thread_p, eval_term1->et.et_comp.rhs,
				vd, NULL, obj_oid, tpl, min);
  if (er_status != NO_ERROR)
    {
      if (er_errid () == NO_ERROR)
	{
	  er_status = ER_QPROC_INVALID_XASLNODE;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, er_status, 0);
	}

      goto error;
    }

  eval_term2 = &pred->rhs->pe.eval_term;
  if (eval_term2->et_type != T_COMP_EVAL_TERM)
    {
      er_status = ER_QPROC_INVALID_XASLNODE;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, er_status, 0);

      goto error;
    }

  /* upper bound, error info is already set in fetch_peek_dbval  */
  er_status = fetch_peek_dbval (thread_p, eval_term2->et.et_comp.rhs,
				vd, NULL, obj_oid, tpl, max);
  if (er_status != NO_ERROR)
    {
      if (er_errid () == NO_ERROR)
	{
	  er_status = ER_QPROC_INVALID_XASLNODE;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, er_status, 0);
	}

      goto error;
    }

error:

  return er_status;
}

/*
 * fetch_copy_dbval () - returns a COPY of a db_value which the caller
 *                    must clear
 *   return: NO_ERROR or ER_code
 *   regu_var(in/out): Regulator Variable
 *   vd(in): Value Descriptor
 *   cls_oid(in): Class Identifier
 *   obj_oid(in): Object Identifier
 *   tpl(in): Tuple
 *   dbval(out): Set to the value resulting from the fetch operation
 *
 * This routine uses the value description indicated by the regulator variable
 * to fetch the indicated value and store it in the dbval parameter.
 * The value may be fetched from either a heap file object instance,
 * or from a list file tuple, or from an all constants regulator variable
 * content. If the value is fetched from a heap file object instance,
 * then tpl parameter should be given NULL. Likewise, if the value is fetched
 * from a list file tuple, then obj_oid parameter should be given NULL.
 * If the value is fetched from all constant values referred to
 * by the regulator variable, then all of the parameters obj_oid,
 * tpl should be given NULL values.
 *
 * If the value description in the regulator variable refers other cases
 * such as constant value, arithmetic expression, the resultant value
 * is computed and stored in the db_value.
 *
 * see fetch_peek_dbval().
 */
int
fetch_copy_dbval (THREAD_ENTRY * thread_p, REGU_VARIABLE * regu_var,
		  VAL_DESCR * vd, OID * class_oid, OID * obj_oid,
		  QFILE_TUPLE tpl, DB_VALUE * dbval)
{
  int result;
  DB_VALUE *readonly_val, copy_val, *tmp;

  db_make_null (&copy_val);

  result = fetch_peek_dbval (thread_p, regu_var, vd, class_oid, obj_oid, tpl,
			     &readonly_val);
  if (result != NO_ERROR)
    {
      return result;
    }

  /*
   * This routine needs to ensure that a copy happens.  If readonly_val
   * points to the same db_value as dbval, qdata_copy_db_value() won't copy.
   * This can happen with scans that are PEEKING and routines that
   * are sending the COPY flag to fetch_val_list() like the group by
   * code.  If this happens we use a stack variable for the copy and
   * then transfer ownership to the returned dbval
   */
  if (dbval == readonly_val)
    {
      tmp = &copy_val;
    }
  else
    {
      tmp = dbval;
    }

  if (!qdata_copy_db_value (tmp, readonly_val))
    {
      result = ER_FAILED;
    }

  if (tmp == &copy_val)
    {
      /*
       * transfer ownership to the real db_value via a
       * structure copy.  Make sure you clear the previous value.
       */
      pr_clear_value (dbval);
      *dbval = *tmp;
    }

  return result;
}

/*
 * fetch_val_list () - fetches all the values for the given regu variable list
 *   return: NO_ERROR or ER_code
 *   regu_list(in/out): Regulator Variable list
 *   vd(in): Value Descriptor
 *   class_oid(in): Class Identifier
 *   obj_oid(in): Object Identifier
 *   tpl(in): Tuple
 *   peek(int):
 */
int
fetch_val_list (THREAD_ENTRY * thread_p, REGU_VARIABLE_LIST regu_list,
		VAL_DESCR * vd, OID * class_oid, OID * obj_oid,
		QFILE_TUPLE tpl, int peek)
{
  REGU_VARIABLE_LIST regup;
  QFILE_TUPLE next_tpl;
  int rc, pos, next_pos;
  DB_VALUE *tmp;

  if (peek)
    {
      next_tpl = tpl + QFILE_TUPLE_LENGTH_SIZE;
      next_pos = 0;

      for (regup = regu_list; regup != NULL; regup = regup->next)
	{
	  if (regup->value.type == TYPE_POSITION)
	    {
	      pos = regup->value.value.pos_descr.pos_no;
	      if (pos >= next_pos)
		{
		  pos -= next_pos;
		  next_pos = regup->value.value.pos_descr.pos_no + 1;
		}
	      else
		{
		  next_tpl = tpl + QFILE_TUPLE_LENGTH_SIZE;
		  next_pos = 0;
		}

	      /* at fetch_peek_dbval_pos(), regup->value.vfetch_to is cleared */
	      rc = fetch_peek_dbval_pos (&regup->value, next_tpl, pos, &tmp,
					 &next_tpl);
	    }
	  else
	    {
	      if (pr_is_set_type
		  (DB_VALUE_DOMAIN_TYPE (regup->value.vfetch_to)))
		{
		  pr_clear_value (regup->value.vfetch_to);
		}
	      rc = fetch_peek_dbval (thread_p, &regup->value, vd, class_oid,
				     obj_oid, tpl, &tmp);
	    }

	  if (rc != NO_ERROR)
	    {
	      pr_clear_value (regup->value.vfetch_to);
	      return ER_FAILED;
	    }
	  PR_SHARE_VALUE (tmp, regup->value.vfetch_to);
	}
    }
  else
    {
      /*
       * These DB_VALUES must persist across object fetches, so we must
       * use fetch_copy_dbval and NOT peek here.
       */
      for (regup = regu_list; regup != NULL; regup = regup->next)
	{
	  if (pr_is_set_type (DB_VALUE_DOMAIN_TYPE (regup->value.vfetch_to)))
	    {
	      pr_clear_value (regup->value.vfetch_to);
	    }
	  if (fetch_copy_dbval (thread_p, &regup->value, vd,
				class_oid, obj_oid, tpl,
				regup->value.vfetch_to) != NO_ERROR)
	    {
	      return ER_FAILED;
	    }
	}
    }

  return NO_ERROR;
}

/*
 * fetch_init_val_list () -
 *   return:
 *   regu_list(in/out): Regulator Variable list
 */
void
fetch_init_val_list (REGU_VARIABLE_LIST regu_list)
{
  REGU_VARIABLE_LIST regu_p;
  REGU_VARIABLE *regu_var;

  for (regu_p = regu_list; regu_p; regu_p = regu_p->next)
    {
      regu_var = &regu_p->value;
      regu_var->value.attr_descr.cache_dbvalp = NULL;
    }
}

/*
 * is_argument_wrapped_with_cast_op () - check if regu_var is a cast
 *					 expression
 *   return: true/false
 *   regu_list(in/out): Regulator Variable list
 *   vd(in): Value Descriptor
 *   class_oid(in): Class Identifier
 *   obj_oid(in): Object Identifier
 *   tpl(in): Tuple
 *   peek(int):
 */
static bool
is_argument_wrapped_with_cast_op (const REGU_VARIABLE * regu_var)
{
  if (regu_var == NULL)
    {
      return false;
    }

  if (regu_var->type == TYPE_INARITH || regu_var->type == TYPE_OUTARITH)
    {
      return (regu_var->value.arithptr->opcode == T_CAST);
    }

  return false;
}
