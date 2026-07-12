/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "executor_abstract.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> sort_cols_;
    std::vector<bool> is_desc_;
    int64_t limit_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_ = 0;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sort_cols,
                 std::vector<bool> is_desc, int64_t limit)
        : prev_(std::move(prev)), is_desc_(std::move(is_desc)), limit_(limit) {
        for (const auto &col : sort_cols) {
            sort_cols_.push_back(prev_->get_col_offset(col));
        }
    }

    void beginTuple() override {
        tuples_.clear();
        cursor_ = 0;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto tuple = prev_->Next();
            if (tuple != nullptr) tuples_.push_back(std::move(tuple));
        }
        if (!sort_cols_.empty()) {
            std::stable_sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
                for (size_t i = 0; i < sort_cols_.size(); ++i) {
                    const auto &col = sort_cols_[i];
                    int cmp = compare_field(lhs->data + col.offset, col.len,
                                            rhs->data + col.offset, col.len, col.type);
                    if (cmp != 0) return is_desc_[i] ? cmp > 0 : cmp < 0;
                }
                return false;
            });
        }
    }

    void nextTuple() override {
        if (!is_end()) ++cursor_;
    }

    bool is_end() const override {
        size_t available = tuples_.size();
        if (limit_ >= 0) available = std::min(available, static_cast<size_t>(limit_));
        return cursor_ >= available;
    }

    std::unique_ptr<RmRecord> Next() override {
        return is_end() ? nullptr : std::make_unique<RmRecord>(*tuples_[cursor_]);
    }

    size_t tupleLen() const override { return prev_->tupleLen(); }
    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }
    ColMeta get_col_offset(const TabCol &target) override { return prev_->get_col_offset(target); }
    std::string getType() override { return "SortExecutor"; }
    Rid &rid() override { return _abstract_rid; }
};
