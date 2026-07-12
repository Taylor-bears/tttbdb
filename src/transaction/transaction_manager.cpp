/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    (void)log_manager;
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_.fetch_add(1));
    }
    txn->set_start_ts(next_timestamp_.fetch_add(1));
    txn->set_state(TransactionState::GROWING);
    std::lock_guard<std::mutex> guard(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    (void)log_manager;
    if (txn == nullptr) return;
    for (auto *write_record : *txn->get_write_set()) delete write_record;
    txn->get_write_set()->clear();
    for (const auto &lock_id : *txn->get_lock_set()) lock_manager_->unlock(txn, lock_id);
    txn->get_lock_set()->clear();
    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    (void)log_manager;
    if (txn == nullptr) return;

    auto make_key = [](const RmRecord &record, const IndexMeta &index) {
        std::vector<char> key(index.col_tot_len);
        int offset = 0;
        for (const auto &col : index.cols) {
            std::memcpy(key.data() + offset, record.data + col.offset, col.len);
            offset += col.len;
        }
        return key;
    };

    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        std::unique_ptr<WriteRecord> write(write_set->back());
        write_set->pop_back();
        const std::string &table_name = write->GetTableName();
        TabMeta &table = sm_manager_->db_.get_table(table_name);
        RmFileHandle *file = sm_manager_->fhs_.at(table_name).get();
        const Rid rid = write->GetRid();

        if (write->GetWriteType() == WType::INSERT_TUPLE) {
            if (!file->is_record(rid)) continue;
            auto current = file->get_record(rid, nullptr);
            for (const auto &index : table.indexes) {
                auto key = make_key(*current, index);
                auto name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                if (!sm_manager_->ihs_.at(name)->delete_entry(key.data(), txn)) {
                    throw InternalError("Index entry missing while undoing insert");
                }
            }
            file->delete_record(rid, nullptr);
        } else if (write->GetWriteType() == WType::DELETE_TUPLE) {
            RmRecord &old_record = write->GetRecord();
            file->insert_record(rid, old_record.data);
            for (const auto &index : table.indexes) {
                auto key = make_key(old_record, index);
                auto name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                sm_manager_->ihs_.at(name)->insert_entry(key.data(), rid, txn);
            }
        } else if (write->GetWriteType() == WType::UPDATE_TUPLE) {
            auto current = file->get_record(rid, nullptr);
            for (const auto &index : table.indexes) {
                auto key = make_key(*current, index);
                auto name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                if (!sm_manager_->ihs_.at(name)->delete_entry(key.data(), txn)) {
                    throw InternalError("Index entry missing while undoing update");
                }
            }
            RmRecord &old_record = write->GetRecord();
            file->update_record(rid, old_record.data, nullptr);
            for (const auto &index : table.indexes) {
                auto key = make_key(old_record, index);
                auto name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                sm_manager_->ihs_.at(name)->insert_entry(key.data(), rid, txn);
            }
        }
    }

    for (const auto &lock_id : *txn->get_lock_set()) lock_manager_->unlock(txn, lock_id);
    txn->get_lock_set()->clear();
    txn->set_state(TransactionState::ABORTED);
}
