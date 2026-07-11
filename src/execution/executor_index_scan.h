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

#include <limits>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    IxIndexHandle *ih_;
    std::vector<char> lower_key_;
    std::vector<char> upper_key_;
    bool lower_strict_{false};
    bool upper_inclusive_{true};
    bool empty_range_{false};

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols);
        ih_ = sm_manager_->ihs_.at(index_name).get();
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
        build_bounds();
    }

    void beginTuple() override {
        if (empty_range_) {
            Iid end = ih_->leaf_end();
            scan_ = std::make_unique<IxScan>(ih_, end, end, sm_manager_->get_bpm());
            return;
        }
        Iid lower = lower_strict_ ? ih_->upper_bound(lower_key_.data()) : ih_->lower_bound(lower_key_.data());
        Iid upper = upper_inclusive_ ? ih_->upper_bound(upper_key_.data()) : ih_->lower_bound(upper_key_.data());
        scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
        seek_next_match();
    }

    void nextTuple() override {
        if (scan_ == nullptr || scan_->is_end()) return;
        scan_->next();
        seek_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    std::string getType() override { return "IndexScanExecutor"; }

    Rid &rid() override { return rid_; }

   private:
    static void fill_extreme(char *dest, const ColMeta &col, bool maximum) {
        if (col.type == TYPE_INT) {
            int value = maximum ? std::numeric_limits<int>::max() : std::numeric_limits<int>::min();
            memcpy(dest, &value, sizeof(value));
        } else if (col.type == TYPE_FLOAT) {
            float value = maximum ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
            memcpy(dest, &value, sizeof(value));
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t value = maximum ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min();
            memcpy(dest, &value, sizeof(value));
        } else {
            memset(dest, maximum ? 0xff : 0x00, col.len);
        }
    }

    void build_bounds() {
        lower_key_.resize(index_meta_.col_tot_len);
        upper_key_.resize(index_meta_.col_tot_len);
        int offset = 0;
        for (const auto &col : index_meta_.cols) {
            fill_extreme(lower_key_.data() + offset, col, false);
            fill_extreme(upper_key_.data() + offset, col, true);
            offset += col.len;
        }

        offset = 0;
        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            const auto &col = index_meta_.cols[i];
            const Condition *equal = nullptr;
            for (const auto &cond : conds_) {
                if (cond.is_rhs_val && cond.lhs_col.tab_name == tab_name_ && cond.lhs_col.col_name == col.name &&
                    cond.op == OP_EQ) {
                    equal = &cond;
                    break;
                }
            }
            if (equal != nullptr) {
                memcpy(lower_key_.data() + offset, equal->rhs_val.raw->data, col.len);
                memcpy(upper_key_.data() + offset, equal->rhs_val.raw->data, col.len);
                offset += col.len;
                continue;
            }

            const Condition *lower = nullptr;
            const Condition *upper = nullptr;
            for (const auto &cond : conds_) {
                if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != col.name) continue;
                if (cond.op == OP_GT || cond.op == OP_GE) {
                    int cmp = lower == nullptr ? 1 : compare_field(cond.rhs_val.raw->data, col.len,
                        lower->rhs_val.raw->data, col.len, col.type);
                    if (lower == nullptr || cmp > 0 || (cmp == 0 && cond.op == OP_GT && lower->op == OP_GE)) lower = &cond;
                } else if (cond.op == OP_LT || cond.op == OP_LE) {
                    int cmp = upper == nullptr ? -1 : compare_field(cond.rhs_val.raw->data, col.len,
                        upper->rhs_val.raw->data, col.len, col.type);
                    if (upper == nullptr || cmp < 0 || (cmp == 0 && cond.op == OP_LT && upper->op == OP_LE)) upper = &cond;
                }
            }
            if (lower != nullptr) {
                memcpy(lower_key_.data() + offset, lower->rhs_val.raw->data, col.len);
                lower_strict_ = lower->op == OP_GT;
                int trailing = offset + col.len;
                if (lower_strict_) {
                    for (size_t j = i + 1; j < index_meta_.cols.size(); ++j) {
                        fill_extreme(lower_key_.data() + trailing, index_meta_.cols[j], true);
                        trailing += index_meta_.cols[j].len;
                    }
                }
            }
            if (upper != nullptr) {
                memcpy(upper_key_.data() + offset, upper->rhs_val.raw->data, col.len);
                upper_inclusive_ = upper->op == OP_LE;
                int trailing = offset + col.len;
                if (!upper_inclusive_) {
                    for (size_t j = i + 1; j < index_meta_.cols.size(); ++j) {
                        fill_extreme(upper_key_.data() + trailing, index_meta_.cols[j], false);
                        trailing += index_meta_.cols[j].len;
                    }
                }
            }
            break;
        }
        int cmp = 0;
        int compare_offset = 0;
        for (const auto &col : index_meta_.cols) {
            cmp = compare_field(lower_key_.data() + compare_offset, col.len,
                                upper_key_.data() + compare_offset, col.len, col.type);
            if (cmp != 0) break;
            compare_offset += col.len;
        }
        empty_range_ = cmp > 0 || (cmp == 0 && (lower_strict_ || !upper_inclusive_));
    }

    void seek_next_match() {
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto record = fh_->get_record(rid_, context_);
            if (satisfies(*record, cols_, fed_conds_)) return;
            scan_->next();
        }
        rid_ = {RM_NO_PAGE, -1};
    }
};
