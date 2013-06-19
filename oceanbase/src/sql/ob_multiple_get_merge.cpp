/**
 * (C) 2010-2012 Alibaba Group Holding Limited.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * Version: $Id$
 *
 * /home/jianming.cjq/ss_g/src/sql/ob_multiple_get_merge.cpp 
 *
 * Authors:
 *   Junquan Chen <jianming.cjq@alipay.com>
 *
 */

#include "ob_multiple_get_merge.h"
#include "common/ob_row_fuse.h"

using namespace oceanbase;
using namespace sql;

int ObMultipleGetMerge::open()
{
  int ret = OB_SUCCESS;
  const ObRowDesc *row_desc = NULL;

  for (int32_t i = 0; OB_SUCCESS == ret && i < child_num_; i ++)
  {
    if (OB_SUCCESS != (ret = child_array_[i]->open()))
    {
      TBSYS_LOG(WARN, "fail to open child i[%d]:ret[%d]", i, ret);
    }
  }

  if (OB_SUCCESS == ret)
  {
    ret = get_row_desc(row_desc);
    if (OB_SUCCESS != ret || NULL == row_desc)
    {
      TBSYS_LOG(WARN, "get row desc fail:ret[%d]", ret);
    }
    else if (row_desc->get_rowkey_cell_count() <= 0)
    {
      ret = OB_ERR_UNEXPECTED;
      TBSYS_LOG(ERROR, "rowkey count[%ld]", row_desc->get_rowkey_cell_count());
    }
    else
    {
      cur_row_.set_row_desc(*row_desc);
    }
  }

  return ret;
}

int ObMultipleGetMerge::close()
{
  int ret = OB_SUCCESS;

  for (int32_t i = 0; i < child_num_; i ++)
  {
    if (OB_SUCCESS != (ret = child_array_[i]->close()))
    {
      TBSYS_LOG(WARN, "fail to close child i[%d]:ret[%d]", i, ret);
    }
  }

  return ret;
}

int ObMultipleGetMerge::get_next_row(const ObRow *&row)
{
  int ret = OB_SUCCESS;
  const ObRow *tmp_row = NULL;
  bool is_row_empty = true;

  if (child_num_ <= 0)
  {
    ret = OB_NOT_INIT;
    TBSYS_LOG(WARN, "has no child");
  }
  cur_row_.reset(false, is_ups_row_ ? ObRow::DEFAULT_NOP : ObRow::DEFAULT_NULL);
  for (int32_t i = 0; OB_SUCCESS == ret && i < child_num_; i++)
  {
    ret = child_array_[i]->get_next_row(tmp_row);
    if (OB_SUCCESS != ret)
    {
      if (OB_ITER_END == ret)
      {
        if (0 != i)
        {
          ret = OB_ERR_UNEXPECTED;
          TBSYS_LOG(ERROR, "child[%d] should not reach iter end first", i);
        }
        else
        {
          int err = OB_SUCCESS;
          for (int32_t k = 1; OB_ITER_END == ret && k < child_num_; ++k)
          { // check end
            err = child_array_[k]->get_next_row(tmp_row);
            if (OB_ITER_END != err)
            {
              ret = OB_ERR_UNEXPECTED;
              TBSYS_LOG(ERROR, "child[0] has reached iter, but child[%d] not", k);
            }
          }
        }
        break;
      }
      else
      {
        TBSYS_LOG(WARN, "fail to get next row:ret[%d]", ret);
      }
    }

    if (OB_SUCCESS == ret)
    {
      TBSYS_LOG(DEBUG, "multiple get merge child[%d] row[%s]", i, to_cstring(*tmp_row));
      if (OB_SUCCESS != (ret = common::ObRowFuse::fuse_row(*tmp_row, cur_row_, is_row_empty, is_ups_row_)))
      {
        TBSYS_LOG(WARN, "fail to fuse row:ret[%d], tmp_row[%s], cur_row_[%s]", ret, to_cstring(*tmp_row), to_cstring(cur_row_));
      }
      else if (0 == i)
      {
        if (OB_SUCCESS != (ret = copy_rowkey(*tmp_row, cur_row_, false)))
        {
          TBSYS_LOG(WARN, "fail to copy rowkey:ret[%d]", ret);
        }
      }
    }
  }
  if (OB_SUCCESS == ret)
  {
    row = &cur_row_;

    ret = cur_row_.get_rowkey(cur_rowkey_);
    if (OB_SUCCESS != ret || NULL == cur_rowkey_)
    {
      TBSYS_LOG(WARN, "fail to get rowkey to update cur rowkey:ret[%d]", ret);
    }
  }
  return ret;
}

int64_t ObMultipleGetMerge::to_string(char *buf, int64_t buf_len) const
{
  int64_t pos = 0;
  databuff_printf(buf, buf_len, pos, "MuitipleGetMerge(children_num=%d)\n",
                  child_num_);
  for (int32_t i = 0; i < child_num_; ++i)
  {
    databuff_printf(buf, buf_len, pos, "Child%d:\n", i);
    if (NULL != child_array_[i])
    {
      pos += child_array_[i]->to_string(buf+pos, buf_len-pos);
    }
  }
  return pos;
}
