/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cstring>
#include <vector>
#include <unistd.h>
#include "log_manager.h"

/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    if (log_record == nullptr) throw InternalError("Cannot append null log record");
    std::lock_guard<std::mutex> guard(latch_);
    log_record->lsn_ = global_lsn_.fetch_add(1);
    if (log_record->log_tot_len_ > LOG_BUFFER_SIZE) {
        if (log_buffer_.offset_ > 0) {
            disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
            log_buffer_.offset_ = 0;
        }
        std::vector<char> serialized(log_record->log_tot_len_);
        log_record->serialize(serialized.data());
        disk_manager_->write_log(serialized.data(), static_cast<int>(serialized.size()));
        return log_record->lsn_;
    }
    if (log_buffer_.is_full(static_cast<int>(log_record->log_tot_len_))) {
        if (log_buffer_.offset_ > 0) {
            disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
            log_buffer_.offset_ = 0;
        }
    }
    log_record->serialize(log_buffer_.buffer_ + log_buffer_.offset_);
    log_buffer_.offset_ += static_cast<int>(log_record->log_tot_len_);
    return log_record->lsn_;
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */
void LogManager::flush_log_to_disk() {
    std::lock_guard<std::mutex> guard(latch_);
    if (log_buffer_.offset_ > 0) {
        disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
        log_buffer_.offset_ = 0;
        persist_lsn_ = global_lsn_.load() - 1;
    }
    if (disk_manager_->GetLogFd() >= 0) disk_manager_->sync_file(disk_manager_->GetLogFd());
}
