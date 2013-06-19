/**
 * (C) 2010-2012 Alibaba Group Holding Limited.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * Version: $Id$
 *
 * ob_row_store.cpp
 *
 * Authors:
 *   Zhifeng YANG <zhuweng.yzf@taobao.com>
 *
 */
#include "ob_row_store.h"
#include "ob_row_util.h"
#include "utility.h"
using namespace oceanbase::common;

////////////////////////////////////////////////////////////////
struct ObRowStore::BlockInfo
{
  BlockInfo()
    :magic_(0xabcd4444abcd4444), next_block_(NULL), curr_data_pos_(0)
  {
  }
  inline int64_t get_remain_size() const
  {
    return ObRowStore::BLOCK_SIZE - curr_data_pos_ - sizeof(BlockInfo);
  }
  inline int64_t get_remain_size_for_read(int64_t pos) const
  {
    //TBSYS_LOG(DEBUG, "cur=%ld, pos=%ld, remain=%ld", curr_data_pos_, pos, curr_data_pos_ - pos);
    return curr_data_pos_ - pos;
  }
  inline char* get_buffer()
  {
    return data_ + curr_data_pos_;
  }
  inline const char* get_buffer_head() const
  {
    return data_;
  }
  inline void advance(const int64_t length)
  {
    curr_data_pos_ += length;
  }
  inline int64_t get_curr_data_pos()
  {
    return curr_data_pos_;
  }
  inline void set_curr_data_pos(int64_t pos)
  {
    curr_data_pos_ = pos;
  }

  BlockInfo *&get_next_block()
  {
    return next_block_;
  }

private:
  int64_t magic_;
  BlockInfo *next_block_;
  /**
   * cur_data_pos_ must be set when BlockInfo deserialized
   */
  int64_t curr_data_pos_;
  char data_[0];
};

////////////////////////////////////////////////////////////////
ObRowStore::ObRowStore(const int32_t mod_id/*=ObModIds::OB_SQL_ROW_STORE*/, ObIAllocator* allocator/*=NULL*/)
  :allocator_(allocator ?: &get_global_tsi_block_allocator()),
   block_list_head_(NULL), block_list_tail_(NULL),
   block_count_(0), cur_size_counter_(0), got_first_next_(false),
   last_prev_row_store_size_(0), last_row_store_size_(0), cur_iter_pos_(0), cur_iter_block_(NULL),
   rollback_iter_pos_(-1), rollback_block_list_(NULL),
   mod_id_(mod_id)
{
}

ObRowStore::~ObRowStore()
{
  clear();
}

int ObRowStore::get_last_stored_row(const StoredRow *&stored_row) const
{
  int ret = OB_SUCCESS;
  if (0 == last_row_store_size_)
  {
    ret = OB_NOT_INIT;
    TBSYS_LOG(ERROR, "has no last stored row");
  }
  else if (block_list_head_->get_curr_data_pos() - last_row_store_size_ < 0)
  {
    ret = OB_ERR_UNEXPECTED;
    TBSYS_LOG(ERROR, "wrong last row store size[%ld], cur_data_pos_[%ld]", last_row_store_size_, block_list_head_->get_curr_data_pos());
  }
  else
  {
    stored_row = reinterpret_cast<const StoredRow *>(block_list_head_->get_buffer_head() + (block_list_head_->get_curr_data_pos() - last_row_store_size_));
  }
  return ret;
}

int ObRowStore::add_reserved_column(uint64_t tid, uint64_t cid)
{
  int ret = OB_SUCCESS;
  if (OB_SUCCESS != (ret = reserved_columns_.push_back(std::make_pair(tid, cid))))
  {
    TBSYS_LOG(WARN, "failed to push into array, err=%d", ret);
  }
  return ret;
}

void ObRowStore::reuse()
{
  block_list_head_ = block_list_tail_;
  if(NULL != block_list_head_)
  {
    block_list_head_->set_curr_data_pos(0);
  }
  reserved_columns_.clear();
  cur_size_counter_ = 0;
  got_first_next_ = false;
  last_prev_row_store_size_ = 0;
  last_row_store_size_ = 0;
  cur_iter_pos_ = 0;
  cur_iter_block_ = NULL;
  rollback_iter_pos_ = -1;
  rollback_block_list_ = NULL;
}

void ObRowStore::clear()
{
  reuse();

  while(NULL != block_list_tail_)
  {
    // using block_list_tail_ as the list iterator can prevent
    // memory leakage when there was a rollback
    BlockInfo *tmp = block_list_tail_->get_next_block();
    ob_tc_free(block_list_tail_, mod_id_);
    block_list_tail_ = tmp;
  }
  block_list_head_ = NULL;
  block_count_ = 0;
}


// method for ObAggregateFunction::prepare()
// prepare need to reuse ObRowStore for WRITE,
// it needs to reuse reserved_columns_ which should not be cleared
void ObRowStore::clear_rows()
{
  block_list_head_ = block_list_tail_;
  if(NULL != block_list_head_)
  {
    block_list_head_->set_curr_data_pos(0);
  }
  cur_size_counter_ = 0;
  got_first_next_ = false;
  last_prev_row_store_size_ = 0;
  last_row_store_size_ = 0;
  cur_iter_pos_ = 0;
  cur_iter_block_ = NULL;
  rollback_iter_pos_ = -1;
  rollback_block_list_ = NULL;
  while(NULL != block_list_tail_)
  {
    // using block_list_tail_ as the list iterator can prevent
    // memory leakage when there was a rollback
    BlockInfo *tmp = block_list_tail_->get_next_block();
    ob_tc_free(block_list_tail_, mod_id_);
    block_list_tail_ = tmp;
  }
  block_list_head_ = NULL;
  block_count_ = 0;
}


int ObRowStore::rollback_last_row()
{
  int ret = OB_SUCCESS;
  // only support add_row->rollback->add_row->rollback->add_row->add_row->rollback
  // NOT support  ..->rollback->rollback->...
  if (rollback_iter_pos_ < 0)
  {
    TBSYS_LOG(WARN, "only one row could be rollback after called add_row() once");
    ret = OB_NOT_SUPPORTED;
  }
  else
  {
    block_list_head_ = rollback_block_list_;
    if (NULL != block_list_head_)
    {
      block_list_head_->set_curr_data_pos(rollback_iter_pos_);
      last_row_store_size_ = last_prev_row_store_size_;
    }
    rollback_iter_pos_ = -1;
  }
  return ret;
}

int ObRowStore::new_block()
{
  int ret = OB_SUCCESS;
  // when row store was ever rolled back, block_list_head_->get_next_block() may not be NULL
  // that block could be reused
  if ((NULL != block_list_head_) && (NULL != block_list_head_->get_next_block()))
  {
    // reuse block
    block_list_head_ = block_list_head_->get_next_block();
    block_list_head_->set_curr_data_pos(0);
  }
  else
  {
    // create a new block
    BlockInfo *block = static_cast<BlockInfo*>(ob_tc_malloc(BLOCK_SIZE, mod_id_));
    if (NULL == block)
    {
      TBSYS_LOG(ERROR, "no memory");
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    else
    {
      block = new (block) BlockInfo();
      block->get_next_block() = NULL;
      block->set_curr_data_pos(0);
      if (NULL == block_list_tail_)
      {
        block_list_tail_ = block;  // this is the first block allocated
      }
      if (NULL == block_list_head_)
      {
        block_list_head_ = block;
      }
      else
      {
        block_list_head_->get_next_block() = block;
        block_list_head_ = block;
      }
      ++block_count_;
    }
  }
  return ret;
}

int ObRowStore::append_extend_cell(ObCompactCellWriter& cell_writer, const ObObj& cell)
{
  int ret = OB_SUCCESS;
  int64_t ext_value = 0;
  if(OB_SUCCESS != (ret = cell.get_ext(ext_value)))
  {
    TBSYS_LOG(WARN, "get ext value fail:ret[%d]", ret);
  }
  else
  {
    int64_t escape = 0;
    switch(ext_value)
    {
      case ObActionFlag::OP_VALID:
        escape = ObCellMeta::ES_VALID;
        break;
      case ObActionFlag::OP_ROW_DOES_NOT_EXIST:
        escape = ObCellMeta::ES_NOT_EXIST_ROW;
        break;
      case ObActionFlag::OP_DEL_ROW:
        escape = ObCellMeta::ES_DEL_ROW;
        break;
      case ObActionFlag::OP_NEW_ADD:
        escape = ObCellMeta::ES_NEW_ADD;
        break;
      case ObActionFlag::OP_NOP:
        escape = ObCellMeta::ES_NOP_ROW;
        break;
      default:
        ret = OB_NOT_SUPPORTED;
        TBSYS_LOG(WARN, "not supported ext value:ext[%ld]", ext_value);
    }
    if (OB_SUCCESS == ret)
    {
      ret = cell_writer.append_escape(escape);
      if (OB_SUCCESS != ret)
      {
        TBSYS_LOG(WARN, "fail to append escape[%ld]:ret[%d]", escape, ret);
      }
    }
  }
  return ret;
}

int ObRowStore::append_row(const ObRow &row, BlockInfo &block, StoredRow &stored_row)
{
  int ret = OB_SUCCESS;
  const ObObj *cell = NULL;
  uint64_t table_id = OB_INVALID_ID;
  uint64_t column_id = OB_INVALID_ID;
  ObObj cell_clone;
  const int64_t reserved_columns_count = reserved_columns_.count();
  ObCompactCellWriter cell_writer;

  cell_writer.init(block.get_buffer(), block.get_remain_size(), SPARSE);


  for (int64_t i = 0; OB_SUCCESS == ret && i < row.get_column_num(); ++i)
  {
    if (OB_SUCCESS != (ret = row.raw_get_cell(i, cell, table_id, column_id)))
    {
      TBSYS_LOG(WARN, "failed to get cell, err=%d", ret);
      break;
    }
    if (OB_SUCCESS == ret)
    {
      if (ObExtendType == cell->get_type())
      {
        ret = append_extend_cell(cell_writer, *cell);
        if (OB_SUCCESS != ret)
        {
          TBSYS_LOG(WARN, "failed to append extend cell, err=%d", ret);
        }
      }
      else if (OB_SUCCESS != (ret = cell_writer.append(column_id, *cell, &cell_clone)))
      {
        if (OB_BUF_NOT_ENOUGH != ret)
        {
          TBSYS_LOG(WARN, "failed to append cell, err=%d", ret);
        }
        break;
      }
    }
    if (OB_SUCCESS == ret)
    {
      // whether reserve this cell
      for (int32_t j = 0; j < reserved_columns_count; ++j)
      {
        const std::pair<uint64_t,uint64_t> &tid_cid = reserved_columns_.at(j);
        if (table_id == tid_cid.first && column_id == tid_cid.second)
        {
          stored_row.reserved_cells_[j] = cell_clone;
          break;
        }
      } // end for j
    }
  } // end for i
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = cell_writer.row_finish()))
    {
      if (OB_BUF_NOT_ENOUGH != ret)
      {
        TBSYS_LOG(WARN, "failed to append cell, err=%d", ret);
      }
    }
    else
    {
      stored_row.compact_row_size_ = static_cast<int32_t>(cell_writer.size());
      block.advance(cell_writer.size());
      last_prev_row_store_size_ = last_row_store_size_;
      last_row_store_size_ = cell_writer.size() + get_reserved_cells_size(stored_row.reserved_cells_count_);
      cur_size_counter_ += cell_writer.size();
    }
  }
  return ret;
}


int ObRowStore::add_row(const ObRow &row, const StoredRow *&stored_row)
{
  int64_t cur_size_counter = 0; // value ignored
  return add_row(row, stored_row, cur_size_counter);
}

int ObRowStore::add_row(const ObRow &row, int64_t &cur_size_counter)
{
  const StoredRow *stored_row = NULL; // value ignored
  return add_row(row, stored_row, cur_size_counter);
}

int ObRowStore::add_row(const ObRow &row, const StoredRow *&stored_row, int64_t &cur_size_counter)
{
  int ret = OB_SUCCESS;
  stored_row = NULL;
  const int32_t reserved_columns_count = static_cast<int32_t>(reserved_columns_.count());

  // in case this row would be rollback
  rollback_block_list_ = block_list_head_;
  rollback_iter_pos_ = ((block_list_head_==NULL) ? (-1) : block_list_head_->get_curr_data_pos());

  if (NULL == block_list_head_
      || block_list_head_->get_remain_size() <= get_compact_row_min_size(row.get_column_num())
      + get_reserved_cells_size(reserved_columns_count))
  {
    ret = new_block();
  }

  int64_t retry = 0;
  while(OB_SUCCESS == ret && retry < 2)
  {
    // append OrderByCells
    OB_ASSERT(block_list_head_);
    StoredRow *reserved_cells = reinterpret_cast<StoredRow*>(block_list_head_->get_buffer());
    reserved_cells->reserved_cells_count_ = static_cast<int32_t>(reserved_columns_count);
    block_list_head_->advance(get_reserved_cells_size(reserved_columns_count));
    if (OB_SUCCESS != (ret = append_row(row, *block_list_head_, *reserved_cells)))
    {
      if (OB_BUF_NOT_ENOUGH == ret)
      {
        // buffer not enough
        block_list_head_->advance( -get_reserved_cells_size(reserved_columns_count) );
        TBSYS_LOG(DEBUG, "block buffer not enough, buff=%p remain_size=%ld block_count=%ld",
                  block_list_head_, block_list_head_->get_remain_size(), block_count_);
        ret = OB_SUCCESS;
        ++retry;
        ret = new_block();
      }
      else
      {
        TBSYS_LOG(WARN, "failed to append row, err=%d", ret);
      }
    }
    else
    {
      cur_size_counter_ += get_reserved_cells_size(reserved_columns_count);
      stored_row = reserved_cells;
      cur_size_counter = cur_size_counter_;
      break;                  // done
    }
  } // end while
  if (2 <= retry)
  {
    ret = OB_ERR_UNEXPECTED;
    TBSYS_LOG(ERROR, "unexpected branch");
  }
  return ret;
}

int ObRowStore::next_iter_pos(BlockInfo *&iter_block, int64_t &iter_pos)
{
  int ret = OB_SUCCESS;
  if (NULL == iter_block)
  {
    ret = OB_ITER_END;
  }
  else
  {
    while (0 >= iter_block->get_remain_size_for_read(iter_pos))
    {
      iter_block = iter_block->get_next_block();
      iter_pos = 0;
      if (NULL == iter_block)
      {
        ret = OB_ITER_END;
        break;
      }
    }
  }
  return ret;
}

void ObRowStore::reset_iterator()
{
  cur_iter_block_ = block_list_tail_;
  cur_iter_pos_ = 0;
  got_first_next_ = false;
}

int ObRowStore::get_next_row(ObRow &row, common::ObString *compact_row /* = NULL */)
{
  int ret = OB_SUCCESS;
  const StoredRow *stored_row = NULL;

  if (!got_first_next_)
  {
    cur_iter_block_ = block_list_tail_;
    cur_iter_pos_ = 0;
    got_first_next_ = true;
  }

  if (OB_ITER_END == (ret = next_iter_pos(cur_iter_block_, cur_iter_pos_)))
  {
    TBSYS_LOG(DEBUG, "iter end.block=%p, pos=%ld", cur_iter_block_, cur_iter_pos_);
  }
  else if (OB_SUCCESS == ret)
  {
    const char *buffer = cur_iter_block_->get_buffer_head() + cur_iter_pos_;
    stored_row = reinterpret_cast<const StoredRow *>(buffer);
    cur_iter_pos_ += (get_reserved_cells_size(stored_row->reserved_cells_count_) + stored_row->compact_row_size_);
    //TBSYS_LOG(DEBUG, "stored_row->reserved_cells_count_=%d, stored_row->compact_row_size_=%d, sizeof(ObObj)=%lu, next_pos_=%ld",
    //stored_row->reserved_cells_count_, stored_row->compact_row_size_, sizeof(ObObj), cur_iter_pos_);

    if (OB_SUCCESS == ret && NULL != stored_row)
    {
      if(OB_SUCCESS != (ret = ObRowUtil::convert(stored_row->get_compact_row(), row)))
      {
        TBSYS_LOG(WARN, "fail to convert compact row to ObRow:ret[%d]", ret);
      }
      else if (NULL != compact_row)
      {
        *compact_row = stored_row->get_compact_row();
      }
    }
    else
    {
      TBSYS_LOG(WARN, "fail to get next row. stored_row=%p, ret=%d", stored_row, ret);
    }
  }

  return ret;
}


int64_t ObRowStore::get_used_mem_size() const
{
  return block_count_ * BLOCK_SIZE;
}


DEFINE_SERIALIZE(ObRowStore)
{
  int ret = OB_SUCCESS;
  ObObj obj;
  BlockInfo *block = block_list_tail_;
  

  while(NULL != block)
  {
    // serialize block size
    obj.set_int(block->get_curr_data_pos());
    if (OB_SUCCESS != (ret = obj.serialize(buf, buf_len, pos)))
    {
      TBSYS_LOG(WARN, "fail to serialize block size. ret=%d", ret);
      break;
    }
    // serialize block data
    else
    {
      if (buf_len - pos < block->get_curr_data_pos())
      {
        TBSYS_LOG(WARN, "buffer not enough.");
        ret = OB_BUF_NOT_ENOUGH;
      }
      else
      {
        memcpy(buf + pos, block->get_buffer_head(), block->get_curr_data_pos());
        pos += block->get_curr_data_pos();
      }
    }
    // serialize next block
    block = block->get_next_block();
    TBSYS_LOG(DEBUG, "serialize next block");
  }

  if (OB_SUCCESS == ret)
  {
    obj.set_ext(ObActionFlag::BASIC_PARAM_FIELD);
    if (OB_SUCCESS != (ret = obj.serialize(buf, buf_len, pos)))
    {
      TBSYS_LOG(WARN, "fail to serialize obj:ret[%d]", ret);
    }
    else if (OB_SUCCESS != (ret = serialization::encode_vi64(buf, buf_len, pos, last_row_store_size_)))
    {
      TBSYS_LOG(WARN, "fail to encode last_row_store_size_:ret[%d]", ret);
    }
  }
  if (OB_SUCCESS == ret)
  {
    obj.set_ext(ObActionFlag::OP_END_FLAG);
    if (OB_SUCCESS != (ret = obj.serialize(buf, buf_len, pos)))
    {
      TBSYS_LOG(WARN, "fail to serialize end flag:ret[%d]", ret);
    }
  }
  return ret;
}


DEFINE_DESERIALIZE(ObRowStore)
{
  int ret = OB_SUCCESS;
  int64_t old_pos = pos;
  int64_t block_size = 0;
  ObObj obj;

  reuse();

  while((OB_SUCCESS == (ret = obj.deserialize(buf, data_len, pos)))
              && (ObExtendType != obj.get_type()))
  {
    // get block data size
    if (ObIntType != obj.get_type())
    {
         TBSYS_LOG(WARN, "ObObj deserialize type error, ret=%d buf=%p data_len=%ld pos=%ld",
                       ret, buf, data_len, pos);
         ret = OB_ERROR;
         break;
    }
    if (OB_SUCCESS != (ret = obj.get_int(block_size)))
    {
      TBSYS_LOG(WARN, "ObObj deserialize error, ret=%d buf=%p data_len=%ld pos=%ld",
          ret, buf, data_len, pos);
      break;
    }
    // copy data to store
    if (OB_SUCCESS != (ret = this->new_block()))
    {
      TBSYS_LOG(WARN, "fail to allocate new block. ret=%d", ret);
      break;
    }
    if (NULL != block_list_head_ && block_size <= block_list_head_->get_remain_size() && block_size  >= 0)
    {
      TBSYS_LOG(DEBUG, "copy data from scanner to row_store. buf=%p, pos=%ld, block_size=%ld",
          buf, pos, block_size);
      memcpy(block_list_head_->get_buffer(), buf + pos, block_size);
      block_list_head_->advance(block_size);
      pos += block_size;
      cur_size_counter_ += block_size;
    }
    else
    {
      TBSYS_LOG(WARN, "fail to deserialize scanner data into new block. block_list_head_=%p, block_size=%ld",
          block_list_head_, block_size);
      ret = OB_ERROR;
      break;
    }
  }

  
  if (OB_SUCCESS == ret)
  {
    if (ObExtendType != obj.get_type() && ObActionFlag::BASIC_PARAM_FIELD != obj.get_ext())
    {
      ret = OB_ERR_UNEXPECTED;
      TBSYS_LOG(WARN, "obj should be BASIC_PARAM_FIELD:obj[%s]", to_cstring(obj));
    }
    else if (OB_SUCCESS != (ret = serialization::decode_vi64(buf, data_len, pos, &last_row_store_size_)))
    {
      TBSYS_LOG(WARN, "fail to decode last_row_store_size_:ret[%d]", ret);
    }
  }

  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = obj.deserialize(buf, data_len, pos)))
    {
      TBSYS_LOG(WARN, "fail to deserialize obj:ret[%d]", ret);
    }
    else if (ObExtendType != obj.get_type() && ObActionFlag::OP_END_FLAG != obj.get_ext())
    {
      ret = OB_ERR_UNEXPECTED;
      TBSYS_LOG(WARN, "obj should be OP_END_FLAG:obj[%s]", to_cstring(obj));
    }
  }

  if (OB_SUCCESS != ret)
  {
    pos = old_pos;
  }

  return ret;
}

DEFINE_GET_SERIALIZE_SIZE(ObRowStore)
{
  int64_t size = 0;
  ObObj obj;
  BlockInfo *block = block_list_tail_;
  while(NULL != block)
  {
    obj.set_int(block->get_curr_data_pos());
    size += obj.get_serialize_size();
    size += block->get_curr_data_pos();
    block = block->get_next_block();
  }
  obj.set_ext(ObActionFlag::BASIC_PARAM_FIELD);
  size += obj.get_serialize_size();
  size += serialization::encoded_length_vi64(last_row_store_size_);
  obj.set_ext(ObActionFlag::OP_END_FLAG);
  size += obj.get_serialize_size();
  return size;
}

int64_t ObRowStore::to_string(char* buf, const int64_t buf_len) const
{
  int64_t pos = 0;
  databuff_printf(buf, buf_len, pos, "data_size=%ld block_count=%ld", cur_size_counter_, block_count_);
  return pos;
}
