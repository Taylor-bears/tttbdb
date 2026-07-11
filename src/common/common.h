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

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include "defs.h"
#include "errors.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

inline int64_t parse_datetime(const std::string &text) {
    if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != ' ' ||
        text[13] != ':' || text[16] != ':') {
        throw InvalidDatetimeError(text);
    }
    const int digit_positions[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    for (int pos : digit_positions) {
        if (text[pos] < '0' || text[pos] > '9') throw InvalidDatetimeError(text);
    }
    auto number = [&](int pos, int len) {
        int result = 0;
        for (int i = 0; i < len; ++i) result = result * 10 + (text[pos + i] - '0');
        return result;
    };
    int year = number(0, 4);
    int month = number(5, 2);
    int day = number(8, 2);
    int hour = number(11, 2);
    int minute = number(14, 2);
    int second = number(17, 2);
    if (year < 1000 || year > 9999 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) {
        throw InvalidDatetimeError(text);
    }
    static const int days_per_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = days_per_month[month];
    bool leap_year = year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
    if (month == 2 && leap_year) ++max_day;
    if (day < 1 || day > max_day) throw InvalidDatetimeError(text);

    int64_t encoded = year;
    encoded = encoded * 100 + month;
    encoded = encoded * 100 + day;
    encoded = encoded * 100 + hour;
    encoded = encoded * 100 + minute;
    encoded = encoded * 100 + second;
    return encoded;
}

inline std::string datetime_to_string(int64_t encoded) {
    int second = static_cast<int>(encoded % 100); encoded /= 100;
    int minute = static_cast<int>(encoded % 100); encoded /= 100;
    int hour = static_cast<int>(encoded % 100); encoded /= 100;
    int day = static_cast<int>(encoded % 100); encoded /= 100;
    int month = static_cast<int>(encoded % 100); encoded /= 100;
    int year = static_cast<int>(encoded);
    std::ostringstream out;
    out << std::setfill('0') << std::setw(4) << year << '-' << std::setw(2) << month << '-'
        << std::setw(2) << day << ' ' << std::setw(2) << hour << ':' << std::setw(2) << minute
        << ':' << std::setw(2) << second;
    return out.str();
}

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        int64_t bigint_val;  // bigint value
        int64_t datetime_val;  // encoded YYYYMMDDHHMMSS
        float float_val;  // float value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_bigint(int64_t bigint_val_) {
        type = TYPE_BIGINT;
        bigint_val = bigint_val_;
    }

    void set_datetime(const std::string &datetime_text) {
        type = TYPE_DATETIME;
        datetime_val = parse_datetime(datetime_text);
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_BIGINT) {
            assert(len == sizeof(int64_t));
            memcpy(raw->data, &bigint_val, sizeof(bigint_val));
        } else if (type == TYPE_DATETIME) {
            assert(len == sizeof(int64_t));
            memcpy(raw->data, &datetime_val, sizeof(datetime_val));
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};
