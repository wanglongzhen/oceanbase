/**
 * (C) 2010-2012 Alibaba Group Holding Limited.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * Version: $Id$
 *
 * ob_multiple_get_merge.h 
 *
 * Authors:
 *   Junquan Chen <jianming.cjq@alipay.com>
 *
 */

#ifndef _OB_MULTIPLE_GET_MERGE_H
#define _OB_MULTIPLE_GET_MERGE_H 1

#include "ob_multiple_merge.h"

namespace oceanbase
{
  namespace sql
  {
    class ObMultipleGetMerge : public ObMultipleMerge
    {
      public:
        int open();
        int close();
        int get_next_row(const ObRow *&row);

        enum ObPhyOperatorType get_type() const{return PHY_MULTIPLE_GET_MERGE;};
        int64_t to_string(char *buf, int64_t buf_len) const;
    };
  }
}

#endif /* _OB_MULTIPLE_GET_MERGE_H */
  

