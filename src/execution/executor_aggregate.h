/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "executor_abstract.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<AggregateSpec> aggregates_;
    std::vector<ColMeta> source_cols_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    bool end_ = true;
    std::unique_ptr<RmRecord> result_;
    Rid rid_{RM_NO_PAGE, -1};

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<AggregateSpec> aggregates)
        : prev_(std::move(prev)), aggregates_(std::move(aggregates)) {
        const auto &input_cols = prev_->cols();
        for (const auto &aggregate : aggregates_) {
            ColMeta source{};
            ColMeta output{};
            if (!aggregate.count_star) {
                source = *get_col(input_cols, aggregate.col);
            }
            output.tab_name = "";
            output.name = aggregate.alias;
            output.offset = static_cast<int>(len_);
            output.index = false;
            if (aggregate.type == AGG_COUNT) {
                output.type = TYPE_INT;
                output.len = sizeof(int);
            } else {
                output.type = source.type;
                output.len = source.len;
            }
            len_ += output.len;
            source_cols_.push_back(source);
            cols_.push_back(output);
        }
    }

    void beginTuple() override {
        result_ = std::make_unique<RmRecord>(static_cast<int>(len_));
        std::memset(result_->data, 0, len_);
        std::vector<bool> initialized(aggregates_.size(), false);
        std::vector<int64_t> integer_values(aggregates_.size(), 0);
        std::vector<double> float_values(aggregates_.size(), 0.0);

        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto input = prev_->Next();
            if (input == nullptr) continue;
            for (size_t i = 0; i < aggregates_.size(); ++i) {
                const auto &aggregate = aggregates_[i];
                const auto &source = source_cols_[i];
                char *destination = result_->data + cols_[i].offset;
                if (aggregate.type == AGG_COUNT) {
                    ++integer_values[i];
                } else if (aggregate.type == AGG_SUM) {
                    const char *data = input->data + source.offset;
                    if (source.type == TYPE_INT) {
                        integer_values[i] += *reinterpret_cast<const int *>(data);
                    } else {
                        float_values[i] += *reinterpret_cast<const float *>(data);
                    }
                } else {
                    const char *data = input->data + source.offset;
                    int comparison = initialized[i]
                        ? compare_field(data, source.len, destination, source.len, source.type)
                        : 0;
                    if (!initialized[i] || (aggregate.type == AGG_MAX && comparison > 0) ||
                        (aggregate.type == AGG_MIN && comparison < 0)) {
                        std::memcpy(destination, data, source.len);
                        initialized[i] = true;
                    }
                }
            }
        }

        for (size_t i = 0; i < aggregates_.size(); ++i) {
            char *destination = result_->data + cols_[i].offset;
            if (aggregates_[i].type == AGG_COUNT) {
                int value = static_cast<int>(integer_values[i]);
                std::memcpy(destination, &value, sizeof(value));
            } else if (aggregates_[i].type == AGG_SUM) {
                if (cols_[i].type == TYPE_INT) {
                    int value = static_cast<int>(integer_values[i]);
                    std::memcpy(destination, &value, sizeof(value));
                } else {
                    float value = static_cast<float>(float_values[i]);
                    std::memcpy(destination, &value, sizeof(value));
                }
            }
        }
        end_ = false;
    }

    void nextTuple() override { end_ = true; }
    bool is_end() const override { return end_; }
    std::unique_ptr<RmRecord> Next() override {
        return end_ ? nullptr : std::make_unique<RmRecord>(*result_);
    }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    std::string getType() override { return "AggregateExecutor"; }
    Rid &rid() override { return rid_; }
};
