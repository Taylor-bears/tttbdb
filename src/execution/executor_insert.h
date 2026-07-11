/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class InsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Value> values_;     // 需要插入的数据
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::string tab_name_;          // 表名称
    Rid rid_;                       // 插入的位置，由于系统默认插入时不指定位置，因此当前rid_在插入后才赋值
    SmManager *sm_manager_;

   public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        if (values.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
    };

    std::unique_ptr<RmRecord> Next() override {
        // Make record buffer
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            if (col.type != val.type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }
        std::vector<std::vector<char>> index_keys;
        std::vector<IxIndexHandle *> index_handles;
        for (const auto &index : tab_.indexes) {
            std::vector<char> key(index.col_tot_len);
            int offset = 0;
            for (const auto &col : index.cols) {
                memcpy(key.data() + offset, rec.data + col.offset, col.len);
                offset += col.len;
            }
            auto name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
            auto handle = sm_manager_->ihs_.at(name).get();
            std::vector<Rid> existing;
            if (handle->get_value(key.data(), &existing, context_->txn_)) throw InternalError("Duplicate index key");
            index_keys.push_back(std::move(key));
            index_handles.push_back(handle);
        }
        // Insert into record file
        rid_ = fh_->insert_record(rec.data, context_);
        size_t inserted = 0;
        try {
            for (; inserted < index_handles.size(); ++inserted) {
                index_handles[inserted]->insert_entry(index_keys[inserted].data(), rid_, context_->txn_);
            }
        } catch (...) {
            for (size_t i = 0; i < inserted; ++i) index_handles[i]->delete_entry(index_keys[i].data(), context_->txn_);
            fh_->delete_record(rid_, context_);
            throw;
        }
        return nullptr;
    }
    Rid &rid() override { return rid_; }
};
