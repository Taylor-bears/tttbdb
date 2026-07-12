/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include "executor_abstract.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    static constexpr size_t DEFAULT_JOIN_BUFFER_BYTES = 4 * 1024 * 1024;

    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;

    size_t block_capacity_;
    std::vector<std::unique_ptr<RmRecord>> left_block_;
    std::unique_ptr<RmRecord> right_record_;
    std::unique_ptr<RmRecord> joined_record_;
    size_t left_pos_ = 0;
    bool is_end_ = true;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                           std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds,
                           size_t join_buffer_bytes = DEFAULT_JOIN_BUFFER_BYTES)
        : left_(std::move(left)), right_(std::move(right)), fed_conds_(std::move(conds)) {
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) col.offset += left_->tupleLen();
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        joined_record_ = std::make_unique<RmRecord>(static_cast<int>(len_));

        size_t left_len = std::max<size_t>(1, left_->tupleLen());
        block_capacity_ = std::max<size_t>(1, join_buffer_bytes / left_len);
        left_block_.reserve(block_capacity_);
    }

    void beginTuple() override {
        is_end_ = false;
        left_->beginTuple();
        if (!load_left_block()) {
            is_end_ = true;
            return;
        }
        right_->beginTuple();
        if (right_->is_end()) {
            is_end_ = true;
            return;
        }
        right_record_ = right_->Next();
        left_pos_ = 0;
        seek_next_match();
    }

    void nextTuple() override {
        if (is_end_) return;
        ++left_pos_;
        seek_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) return nullptr;
        return std::make_unique<RmRecord>(*joined_record_);
    }

    bool is_end() const override { return is_end_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    std::string getType() override { return "BlockNestedLoopJoinExecutor"; }
    Rid &rid() override { return _abstract_rid; }

   private:
    bool load_left_block() {
        left_block_.clear();
        while (!left_->is_end() && left_block_.size() < block_capacity_) {
            auto record = left_->Next();
            if (record != nullptr) left_block_.push_back(std::move(record));
            left_->nextTuple();
        }
        return !left_block_.empty();
    }

    void fill_joined_record(const RmRecord &left_record, const RmRecord &right_record) {
        std::memcpy(joined_record_->data, left_record.data, left_->tupleLen());
        std::memcpy(joined_record_->data + left_->tupleLen(), right_record.data, right_->tupleLen());
    }

    void seek_next_match() {
        while (true) {
            while (!right_->is_end()) {
                while (left_pos_ < left_block_.size()) {
                    fill_joined_record(*left_block_[left_pos_], *right_record_);
                    if (satisfies(*joined_record_, cols_, fed_conds_)) return;
                    ++left_pos_;
                }
                right_->nextTuple();
                if (!right_->is_end()) right_record_ = right_->Next();
                left_pos_ = 0;
            }

            if (!load_left_block()) {
                right_record_.reset();
                is_end_ = true;
                return;
            }
            right_->beginTuple();
            if (right_->is_end()) {
                right_record_.reset();
                is_end_ = true;
                return;
            }
            right_record_ = right_->Next();
            left_pos_ = 0;
        }
    }
};
