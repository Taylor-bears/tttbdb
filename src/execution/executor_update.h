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
#include <set>
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        struct IndexChange {
            IxIndexHandle *handle;
            std::vector<char> old_key;
            std::vector<char> new_key;
            bool changed;
        };
        struct PendingUpdate {
            Rid rid;
            std::unique_ptr<RmRecord> old_record;
            std::unique_ptr<RmRecord> record;
            std::vector<IndexChange> changes;
        };
        std::vector<PendingUpdate> pending;
        std::vector<std::set<std::string>> final_keys(tab_.indexes.size());

        for (const auto &rid : rids_) {
            auto old_record = fh_->get_record(rid, context_);
            auto record = std::make_unique<RmRecord>(*old_record);
            for (const auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                if (set_clause.rhs.raw == nullptr) throw InternalError("SET value is not initialized");
                memcpy(record->data + col->offset, set_clause.rhs.raw->data, col->len);
            }
            std::vector<IndexChange> changes;
            for (size_t index_no = 0; index_no < tab_.indexes.size(); ++index_no) {
                const auto &index = tab_.indexes[index_no];
                IndexChange change;
                change.old_key.resize(index.col_tot_len);
                change.new_key.resize(index.col_tot_len);
                int offset = 0;
                for (const auto &col : index.cols) {
                    memcpy(change.old_key.data() + offset, old_record->data + col.offset, col.len);
                    memcpy(change.new_key.data() + offset, record->data + col.offset, col.len);
                    offset += col.len;
                }
                auto name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                change.handle = sm_manager_->ihs_.at(name).get();
                change.changed = change.old_key != change.new_key;
                std::string final_key(change.new_key.data(), change.new_key.size());
                if (!final_keys[index_no].insert(final_key).second) {
                    throw InternalError("Duplicate index key");
                }
                changes.push_back(std::move(change));
            }
            pending.push_back({rid, std::move(old_record), std::move(record), std::move(changes)});
        }

        auto is_target = [&](const Rid &rid) {
            return std::find(rids_.begin(), rids_.end(), rid) != rids_.end();
        };
        for (auto &item : pending) {
            for (auto &change : item.changes) {
                if (!change.changed) continue;
                std::vector<Rid> existing;
                if (change.handle->get_value(change.new_key.data(), &existing, context_->txn_) &&
                    !existing.empty() && existing.front() != item.rid && !is_target(existing.front())) {
                    throw InternalError("Duplicate index key");
                }
            }
        }
        for (auto &item : pending) {
            for (auto &change : item.changes) {
                if (change.changed && !change.handle->delete_entry(change.old_key.data(), context_->txn_)) {
                    throw InternalError("Index entry missing during update");
                }
            }
        }
        for (auto &item : pending) fh_->update_record(item.rid, item.record->data, context_);
        for (auto &item : pending) {
            for (auto &change : item.changes) {
                if (change.changed) change.handle->insert_entry(change.new_key.data(), item.rid, context_->txn_);
            }
        }
        if (context_->txn_ != nullptr) {
            for (const auto &item : pending) {
                context_->txn_->append_write_record(
                    new WriteRecord(WType::UPDATE_TUPLE, tab_name_, item.rid, *item.old_record));
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
