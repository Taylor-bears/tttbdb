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
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

class AbstractExecutor {
   public:
    Rid _abstract_rid;

    Context *context_;

    virtual ~AbstractExecutor() = default;

    virtual size_t tupleLen() const { return 0; };

    virtual const std::vector<ColMeta> &cols() const {
        std::vector<ColMeta> *_cols = nullptr;
        return *_cols;
    };

    virtual std::string getType() { return "AbstractExecutor"; };

    virtual void beginTuple(){};

    virtual void nextTuple(){};

    virtual bool is_end() const { return true; };

    virtual Rid &rid() = 0;

    virtual std::unique_ptr<RmRecord> Next() = 0;

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta();};

    static int compare_field(const char *lhs, int lhs_len, const char *rhs, int rhs_len, ColType type) {
        if (type == TYPE_INT) {
            int l = *reinterpret_cast<const int *>(lhs);
            int r = *reinterpret_cast<const int *>(rhs);
            return (l > r) - (l < r);
        }
        if (type == TYPE_BIGINT) {
            int64_t l;
            int64_t r;
            std::memcpy(&l, lhs, sizeof(l));
            std::memcpy(&r, rhs, sizeof(r));
            return (l > r) - (l < r);
        }
        if (type == TYPE_DATETIME) {
            int64_t l;
            int64_t r;
            std::memcpy(&l, lhs, sizeof(l));
            std::memcpy(&r, rhs, sizeof(r));
            return (l > r) - (l < r);
        }
        if (type == TYPE_FLOAT) {
            float l = *reinterpret_cast<const float *>(lhs);
            float r = *reinterpret_cast<const float *>(rhs);
            return (l > r) - (l < r);
        }
        int lhs_size = 0;
        while (lhs_size < lhs_len && lhs[lhs_size] != '\0') ++lhs_size;
        int rhs_size = 0;
        while (rhs_size < rhs_len && rhs[rhs_size] != '\0') ++rhs_size;
        int common_size = std::min(lhs_size, rhs_size);
        int cmp = std::memcmp(lhs, rhs, common_size);
        if (cmp == 0) cmp = (lhs_size > rhs_size) - (lhs_size < rhs_size);
        return (cmp > 0) - (cmp < 0);
    }

    static bool compare_result(int cmp, CompOp op) {
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
        }
        return false;
    }

    static bool satisfies(const RmRecord &record, const std::vector<ColMeta> &cols,
                          const std::vector<Condition> &conds) {
        for (const auto &cond : conds) {
            auto lhs = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
                return col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name;
            });
            if (lhs == cols.end()) throw ColumnNotFoundError(cond.lhs_col.col_name);

            const char *rhs_data;
            ColType rhs_type;
            int rhs_len;
            if (cond.is_rhs_val) {
                if (cond.rhs_val.raw == nullptr) throw InternalError("Condition value is not initialized");
                rhs_data = cond.rhs_val.raw->data;
                rhs_type = cond.rhs_val.type;
                rhs_len = cond.rhs_val.raw->size;
            } else {
                auto rhs = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
                    return col.tab_name == cond.rhs_col.tab_name && col.name == cond.rhs_col.col_name;
                });
                if (rhs == cols.end()) throw ColumnNotFoundError(cond.rhs_col.col_name);
                rhs_data = record.data + rhs->offset;
                rhs_type = rhs->type;
                rhs_len = rhs->len;
            }
            if (lhs->type != rhs_type || (lhs->type != TYPE_STRING && lhs->len != rhs_len)) {
                throw IncompatibleTypeError(coltype2str(lhs->type), coltype2str(rhs_type));
            }
            int cmp = compare_field(record.data + lhs->offset, lhs->len, rhs_data, rhs_len, lhs->type);
            if (!compare_result(cmp, cond.op)) return false;
        }
        return true;
    }

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }
};
