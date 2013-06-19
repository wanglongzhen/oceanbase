/*
 * (C) 2007-2010 Taobao Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *
 *
 * Version: 0.1: ob_tablet_manager.h,v 0.1 2010/08/19 10:40:34 chuanhui Exp $
 *
 * Authors:
 *   chuanhui <rizhao.ych@taobao.com>
 *     - some work details if you want
 *
 */
#ifndef __OCEANBASE_CHUNKSERVER_OB_TABLET_MANAGER_H__
#define __OCEANBASE_CHUNKSERVER_OB_TABLET_MANAGER_H__

#include "common/thread_buffer.h"
#include "common/ob_file_client.h"
#include "sstable/ob_blockcache.h"
#include "sstable/ob_block_index_cache.h"
#include "sstable/ob_sstable_row_cache.h"
#include "sstable/ob_sstable_scanner.h"
#include "sstable/ob_sstable_getter.h"
#include "sstable/ob_sstable_reader.h"
#include "compactsstablev2/ob_compact_sstable_getter.h"
#include "compactsstablev2/ob_sstable_block_cache.h"
#include "compactsstablev2/ob_sstable_block_index_cache.h"
#include "sql/ob_sstable_scan.h"
#include "sql/ob_sql_get_param.h"
#include "sql/ob_sql_get_simple_param.h"
#include "ob_join_cache.h"
#include "ob_fileinfo_cache.h"
#include "ob_disk_manager.h"
#include "ob_tablet_image.h"
#include "ob_chunk_server_config.h"
#include "ob_chunk_server_stat.h"
#include "ob_chunk_merge.h"
#include "ob_compactsstable_cache.h"
#include "ob_multi_tablet_merger.h"
#include "ob_bypass_sstable_loader.h"
#include "ob_file_recycle.h"
#include "ob_chunk_log_manager.h"
#include "index/ob_build_index_thread.h"

namespace oceanbase
{
  namespace common
  {
    class ObSchemaManager;
    class ObSchemaManagerV2;
    class ObGetParam;
    class ObScanParam;
    class ObScanner;
    class ObNewRange;
    class ObTabletReportInfo;
    class ObTabletReportInfoList;
    class ObScanner;
    class ObServer;
  }

  namespace sql
  {
    class ObSSTableGetter;
  }

  namespace chunkserver
  {
    struct ObGetThreadContext
    {
      ObTablet* tablets_[common::OB_MAX_GET_ROW_NUMBER];
      int64_t tablets_count_;
      int64_t min_compactsstable_version_;

      sstable::ObBlockIndexCache *block_index_cache_;
      sstable::ObBlockCache *block_cache_;
      sstable::ObSSTableRowCache *row_cache_;

      compactsstablev2::ObCompactSSTableGetter::ObCompactGetThreadContext compact_getter_;

      int64_t readers_count_;
      sstable::ObSSTableReader* readers_[common::OB_MAX_GET_ROW_NUMBER];

      ObGetThreadContext() : tablets_count_(0), block_index_cache_(NULL),
      block_cache_(NULL), row_cache_(NULL), readers_count_(0)
      {
      }
    };


    template <typename T>
    class ObGetParamDecorator {};

    template <>
    class ObGetParamDecorator<common::ObGetParam>
    {
    public:
      ObGetParamDecorator(const common::ObGetParam &param) : param_(param)
      {
      }

      const common::ObRowkey &get_rowkey(int64_t row_index) const
      {
        return param_[param_.get_row_index()[row_index].offset_]->row_key_;
      }

      int64_t get_row_size() const
      {
        return param_.get_row_size();
      }

      uint64_t get_table_id(int64_t row_index) const
      {
        return param_[param_.get_row_index()[row_index].offset_]->table_id_;
      }

      int64_t get_query_version(void) const
      {
        return param_.get_version_range().get_query_version();
      }

      bool is_valid(void) const
      {
        return get_row_size() > 0 && get_query_version() >= 0;
      }

    private:
      const common::ObGetParam &param_;
    };

    template <>
    class ObGetParamDecorator<sql::ObSqlGetParam>
    {
    public:
      ObGetParamDecorator(const sql::ObSqlGetParam &param) : param_(param)
      {
      }

      const common::ObRowkey &get_rowkey(int64_t row_index) const
      {
        return *param_[row_index];
      }

      int64_t get_row_size() const
      {
        return param_.get_row_size();
      }

      uint64_t get_table_id(int64_t row_index) const
      {
        UNUSED(row_index);
        return param_.get_table_id();
      }

      int64_t get_query_version(void) const
      {
        int64_t version = param_.get_data_version();
        return version == OB_NEWEST_DATA_VERSION ? 0 : version;
      }

      bool is_valid(void) const
      {
        return get_row_size() > 0 && get_query_version() >= 0;
      }

    private:
      const sql::ObSqlGetParam &param_;
    };

    template <>
    class ObGetParamDecorator<sql::ObSqlGetSimpleParam>
    {
    public:
      ObGetParamDecorator(const sql::ObSqlGetSimpleParam &param) : param_(param)
      {
      }

      const common::ObRowkey &get_rowkey(int64_t row_index) const
      {
        return *param_[row_index];
      }

      int64_t get_row_size() const
      {
        return param_.get_row_size();
      }

      uint64_t get_table_id(int64_t row_index) const
      {
        UNUSED(row_index);
        return param_.get_table_id();
      }

      int64_t get_query_version(void) const
      {
        int64_t version = param_.get_data_version();
        return version == OB_NEWEST_DATA_VERSION ? 0 : version;
      }

      bool is_valid(void) const
      {
        return get_row_size() > 0 && get_query_version() >= 0;
      }

    private:
      const sql::ObSqlGetSimpleParam &param_;
    };

    class ObChunkServerParam;

    class ObTabletManager
    {
      public:
        static const int32_t MAX_COMMAND_LENGTH = 1024*2;
      private:
        DISALLOW_COPY_AND_ASSIGN(ObTabletManager);

      public:
        ObTabletManager();
        ~ObTabletManager();
        int init(const ObChunkServerConfig* config);
        int init(const int64_t block_cache_size,
            const int64_t block_index_cache_size,
            const int64_t sstable_row_cache_size,
            const int64_t file_info_cache_num,
            const char* data_dir,
            const int64_t max_sstable_size);
        int start_merge_thread();
        int start_bypass_loader_thread();
        int start_build_index_thread();
        int load_tablets();
        void destroy();

      public:
        // get timestamp of current serving tablets
        int64_t get_last_not_merged_version(void) const;

        int prepare_merge_tablets(const int64_t memtable_frozen_version);
        int prepare_tablet_image(const int64_t memtable_frozen_version);

        int merge_tablets(const int64_t memtable_frozen_version);
        ObChunkMerge &get_chunk_merge() ;
        ObBypassSSTableLoader& get_bypass_sstable_loader();
        ObBuildIndexThread& get_build_index_thread();

        int report_tablets();

        int delete_tablet_on_rootserver(ObTablet* delete_tablets[], const int32_t size);
        int uninstall_disk(int32_t disk_no);
        int install_disk(int32_t disk_no);
        int report_capacity_info();

        int create_tablet(const common::ObNewRange& range, const int64_t data_version);

        int migrate_tablet(const common::ObNewRange& range,
            const common::ObServer& dest_server,
            char (*src_path)[common::OB_MAX_FILE_NAME_LENGTH],
            char (*dest_path)[common::OB_MAX_FILE_NAME_LENGTH],
            int64_t & num_file,
            int64_t & tablet_version,
            int64_t& tablet_seq_num,
            int32_t & dest_disk_no,
            uint64_t & crc_sum);

        int dest_load_tablet(const common::ObNewRange& range,
            char (*dest_path)[common::OB_MAX_FILE_NAME_LENGTH],
            const int64_t num_file,
            const int64_t tablet_version,
            const int64_t tablet_seq_num,
            const int32_t dest_disk_no,
            const uint64_t crc_sum);

        void start_gc(const int64_t recycle_version);

        int merge_multi_tablets(common::ObTabletReportInfoList& tablet_list);

        int sync_all_tablet_images();

        int load_bypass_sstables(const common::ObTableImportInfoList& table_list);
        int load_bypass_sstables_over(const common::ObTableImportInfoList& table_list,
          const bool is_load_succ);

        int delete_table(const uint64_t table_id);

      public:
        inline FileInfoCache& get_fileinfo_cache();
        inline sstable::ObBlockCache& get_block_cache();
        inline sstable::ObBlockIndexCache& get_block_index_cache();
        inline compactsstablev2::ObSSTableBlockIndexCache & get_compact_block_index_cache();
        inline compactsstablev2::ObSSTableBlockCache & get_compact_block_cache();
        inline ObMultiVersionTabletImage& get_serving_tablet_image();
        inline ObDiskManager& get_disk_manager();
        inline ObRegularRecycler& get_regular_recycler();
        inline ObScanRecycler& get_scan_recycler();
        inline ObJoinCache& get_join_cache();
        inline void build_scan_context(sql::ScanContext& scan_context);

        const ObMultiVersionTabletImage& get_serving_tablet_image() const;
        inline sstable::ObSSTableRowCache* get_row_cache() const { return sstable_row_cache_; }

      public:
        int dump();

      public:
        inline bool is_stoped() { return !is_init_; }
        inline bool is_disk_maintain() {return is_disk_maintain_;}

        int end_get();
        ObGetThreadContext*& get_cur_thread_get_context();
        int gen_sstable_getter_context(const ObSqlGetSimpleParam& sql_get_param, const chunkserver::ObGetThreadContext* &context, int64_t &tablet_version, int16_t &sstable_version);
        int gen_sstable_getter_context(const ObSqlGetParam &sql_get_param, const chunkserver::ObGetThreadContext* &context, int64_t &tablet_version, int16_t &sstable_version);


      private:
        int init_sstable_scanner(const common::ObScanParam& scan_param,
            const ObTablet* tablet, sstable::ObSSTableScanner& sstable_scanner);

        template <typename T>
        int acquire_tablet(const T &param, ObMultiVersionTabletImage& image,
                           ObTablet* tablets[], int64_t& size,
                           int64_t& tablet_version, int64_t* compactsstable_version = NULL);
        int release_tablet(ObMultiVersionTabletImage& image, ObTablet* tablets[], int64_t size);

        int init_sstable_getter_context(ObGetThreadContext *get_context);

        int init_compact_sstable_getter_context(ObGetThreadContext *get_context);

        int fill_get_data(common::ObIterator& iterator, common::ObScanner& scanner);

      public:

        int fill_tablet_info(const ObTablet& tablet, common::ObTabletReportInfo& tablet_info);
        int send_tablet_report(const common::ObTabletReportInfoList& tablets, bool has_more);

      public:
        // allocate new sstable file sequence, call after load_tablets();
        int64_t allocate_sstable_file_seq();
        // switch to new tablets, call by merge_tablets() after all new tablets loaded.
        int switch_cache();

      private:
        static const int64_t DEF_MAX_TABLETS_NUM  = 4000; // max tablets num
        static const int64_t MAX_RANGE_DUMP_SIZE = 256; // for log

      private:
        enum TabletMgrStatus
        {
          NORMAL = 0, // normal status
          MERGING,    // during daily merging process
          MERGED,     // merging complete, waiting to be switched
        };



      private:
        bool is_init_;
        volatile uint64_t mgr_status_;
        volatile uint64_t max_sstable_file_seq_;
        volatile bool is_disk_maintain_;

        FileInfoCache fileinfo_cache_;
        sstable::ObBlockCache block_cache_;
        sstable::ObBlockIndexCache block_index_cache_;
        compactsstablev2::ObSSTableBlockIndexCache compact_block_index_cache_;
        compactsstablev2::ObSSTableBlockCache compact_block_cache_;
        ObJoinCache join_cache_; //used for join phase of daily merge
        sstable::ObSSTableRowCache* sstable_row_cache_;

        ObDiskManager disk_manager_;
        ObRegularRecycler regular_recycler_;
        ObScanRecycler scan_recycler_;
        ObMultiVersionTabletImage tablet_image_;
        ObChunkLogManager log_manager_;

        ObChunkMerge chunk_merge_;
        const ObChunkServerConfig* config_;
        ObBypassSSTableLoader bypass_sstable_loader_;
        ObBuildIndexThread build_index_thread_;
    };

    inline FileInfoCache&  ObTabletManager::get_fileinfo_cache()
    {
       return fileinfo_cache_;
    }

    inline sstable::ObBlockCache& ObTabletManager::get_block_cache()
    {
       return block_cache_;
    }

    inline sstable::ObBlockIndexCache& ObTabletManager::get_block_index_cache()
    {
      return block_index_cache_;
    }

    inline ObMultiVersionTabletImage& ObTabletManager::get_serving_tablet_image()
    {
      return tablet_image_;
    }

    inline const ObMultiVersionTabletImage& ObTabletManager::get_serving_tablet_image() const
    {
      return tablet_image_;
    }

    inline ObDiskManager& ObTabletManager::get_disk_manager()
    {
      return disk_manager_;
    }

    inline ObRegularRecycler& ObTabletManager::get_regular_recycler()
    {
      return regular_recycler_;
    }

    inline ObScanRecycler& ObTabletManager::get_scan_recycler()
    {
      return scan_recycler_;
    }

    inline ObJoinCache& ObTabletManager::get_join_cache()
    {
      return join_cache_;
    }

    inline compactsstablev2::ObSSTableBlockIndexCache & ObTabletManager::get_compact_block_index_cache()
    {
      return compact_block_index_cache_;
    }

    inline compactsstablev2::ObSSTableBlockCache & ObTabletManager::get_compact_block_cache()
    {
      return compact_block_cache_;
    }

    inline void ObTabletManager::build_scan_context(sql::ScanContext& scan_context)
    {
      scan_context.tablet_image_ = &tablet_image_;
      scan_context.block_index_cache_ = &block_index_cache_;
      scan_context.block_cache_ = &block_cache_;
      scan_context.compact_context_.block_index_cache_ = &compact_block_index_cache_;
      scan_context.compact_context_.block_cache_ = &compact_block_cache_;
      scan_context.build_index_thread_ = &build_index_thread_;
    }

  }
}


#endif //__OB_TABLET_MANAGER_H__
