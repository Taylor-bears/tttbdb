/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "lock_manager.h"

#include <algorithm>

bool LockManager::compatible(LockMode requested, LockMode granted) {
    if (requested == LockMode::EXLUCSIVE || granted == LockMode::EXLUCSIVE) return false;
    if (requested == LockMode::SHARED) {
        return granted == LockMode::SHARED || granted == LockMode::INTENTION_SHARED;
    }
    if (granted == LockMode::SHARED) {
        return requested == LockMode::SHARED || requested == LockMode::INTENTION_SHARED;
    }
    if (requested == LockMode::S_IX || granted == LockMode::S_IX) {
        return requested == LockMode::INTENTION_SHARED || granted == LockMode::INTENTION_SHARED;
    }
    return true;
}

LockManager::GroupLockMode LockManager::to_group_mode(LockMode mode) {
    switch (mode) {
        case LockMode::SHARED: return GroupLockMode::S;
        case LockMode::EXLUCSIVE: return GroupLockMode::X;
        case LockMode::INTENTION_SHARED: return GroupLockMode::IS;
        case LockMode::INTENTION_EXCLUSIVE: return GroupLockMode::IX;
        case LockMode::S_IX: return GroupLockMode::SIX;
    }
    return GroupLockMode::NON_LOCK;
}

void LockManager::refresh_group_mode(LockRequestQueue &queue) {
    queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
    for (const auto &request : queue.request_queue_) {
        if (!request.granted_) continue;
        GroupLockMode mode = to_group_mode(request.lock_mode_);
        if (mode == GroupLockMode::X || queue.group_lock_mode_ == GroupLockMode::X) {
            queue.group_lock_mode_ = GroupLockMode::X;
        } else if (mode == GroupLockMode::SIX || queue.group_lock_mode_ == GroupLockMode::SIX ||
                   (mode == GroupLockMode::S && queue.group_lock_mode_ == GroupLockMode::IX) ||
                   (mode == GroupLockMode::IX && queue.group_lock_mode_ == GroupLockMode::S)) {
            queue.group_lock_mode_ = GroupLockMode::SIX;
        } else if (mode == GroupLockMode::S || queue.group_lock_mode_ == GroupLockMode::S) {
            queue.group_lock_mode_ = GroupLockMode::S;
        } else if (mode == GroupLockMode::IX || queue.group_lock_mode_ == GroupLockMode::IX) {
            queue.group_lock_mode_ = GroupLockMode::IX;
        } else {
            queue.group_lock_mode_ = GroupLockMode::IS;
        }
    }
}

bool LockManager::lock(Transaction *txn, const LockDataId &lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) return true;
    std::lock_guard<std::mutex> guard(latch_);
    if (txn->get_state() != TransactionState::GROWING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    auto &queue = lock_table_[lock_data_id];
    auto own = std::find_if(queue.request_queue_.begin(), queue.request_queue_.end(), [&](const LockRequest &request) {
        return request.txn_id_ == txn->get_transaction_id() && request.granted_;
    });
    if (own != queue.request_queue_.end()) {
        if (own->lock_mode_ == lock_mode || own->lock_mode_ == LockMode::EXLUCSIVE ||
            (own->lock_mode_ == LockMode::S_IX && lock_mode != LockMode::EXLUCSIVE)) {
            return true;
        }
        for (const auto &request : queue.request_queue_) {
            if (request.granted_ && request.txn_id_ != txn->get_transaction_id() &&
                !compatible(lock_mode, request.lock_mode_)) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
        }
        own->lock_mode_ = lock_mode;
        refresh_group_mode(queue);
        return true;
    }

    for (const auto &request : queue.request_queue_) {
        if (request.granted_ && request.txn_id_ != txn->get_transaction_id() &&
            !compatible(lock_mode, request.lock_mode_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }
    queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
    queue.request_queue_.back().granted_ = true;
    refresh_group_mode(queue);
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

bool LockManager::lock_shared_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

bool LockManager::lock_shared_on_table(Transaction *txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction *txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction *txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction *txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::unlock(Transaction *txn, LockDataId lock_data_id) {
    if (txn == nullptr) return true;
    std::lock_guard<std::mutex> guard(latch_);
    auto table_it = lock_table_.find(lock_data_id);
    if (table_it == lock_table_.end()) return false;
    auto &queue = table_it->second;
    auto request = std::find_if(queue.request_queue_.begin(), queue.request_queue_.end(), [&](const LockRequest &item) {
        return item.txn_id_ == txn->get_transaction_id();
    });
    if (request == queue.request_queue_.end()) return false;
    queue.request_queue_.erase(request);
    if (queue.request_queue_.empty()) {
        lock_table_.erase(table_it);
    } else {
        refresh_group_mode(queue);
    }
    return true;
}
