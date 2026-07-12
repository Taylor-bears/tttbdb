/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "log_recovery.h"

#include <cstring>
#include <vector>

void RecoveryManager::analyze() {
    records_.clear();
    committed_.clear();
    aborted_.clear();
    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) return;
    std::vector<char> data(file_size);
    int bytes = disk_manager_->read_log(data.data(), file_size, 0);
    if (bytes <= 0) return;

    int offset = 0;
    while (offset + LOG_HEADER_SIZE <= bytes) {
        LogType type;
        lsn_t lsn;
        uint32_t total_len;
        txn_id_t txn_id;
        std::memcpy(&type, data.data() + offset + OFFSET_LOG_TYPE, sizeof(type));
        std::memcpy(&lsn, data.data() + offset + OFFSET_LSN, sizeof(lsn));
        std::memcpy(&total_len, data.data() + offset + OFFSET_LOG_TOT_LEN, sizeof(total_len));
        std::memcpy(&txn_id, data.data() + offset + OFFSET_LOG_TID, sizeof(txn_id));
        if (total_len < LOG_HEADER_SIZE || offset + static_cast<int>(total_len) > bytes ||
            type < LogType::UPDATE || type > LogType::ABORT) {
            break;
        }

        RecoveryRecord record{type, lsn, txn_id};
        if (type == LogType::UPDATE || type == LogType::INSERT || type == LogType::DELETE) {
            int cursor = offset + OFFSET_LOG_DATA;
            int end = offset + static_cast<int>(total_len);
            auto read_u32 = [&](uint32_t &value) {
                if (cursor + static_cast<int>(sizeof(value)) > end) return false;
                std::memcpy(&value, data.data() + cursor, sizeof(value));
                cursor += sizeof(value);
                return true;
            };
            uint32_t table_len = 0, old_len = 0, new_len = 0;
            if (!read_u32(table_len) || cursor + static_cast<int>(table_len) > end) break;
            record.table_name.assign(data.data() + cursor, table_len);
            cursor += table_len;
            if (cursor + static_cast<int>(sizeof(Rid)) > end) break;
            std::memcpy(&record.rid, data.data() + cursor, sizeof(Rid));
            cursor += sizeof(Rid);
            if (!read_u32(old_len) || cursor + static_cast<int>(old_len) > end) break;
            record.old_data.assign(data.data() + cursor, data.data() + cursor + old_len);
            cursor += old_len;
            if (!read_u32(new_len) || cursor + static_cast<int>(new_len) > end) break;
            record.new_data.assign(data.data() + cursor, data.data() + cursor + new_len);
            cursor += new_len;
            if (cursor != end) break;
        } else if (type == LogType::commit) {
            committed_.insert(txn_id);
        } else if (type == LogType::ABORT) {
            aborted_.insert(txn_id);
        }
        records_.push_back(std::move(record));
        offset += total_len;
    }
}

void RecoveryManager::apply_record(const RecoveryRecord &record, bool use_new_value) {
    if (record.table_name.empty() || !sm_manager_->db_.is_table(record.table_name)) return;
    auto *file = sm_manager_->fhs_.at(record.table_name).get();
    const std::vector<char> &desired = use_new_value ? record.new_data : record.old_data;
    if (desired.empty()) {
        if (file->is_record(record.rid)) file->delete_record(record.rid, nullptr);
        return;
    }
    if (desired.size() != static_cast<size_t>(file->get_file_hdr().record_size)) return;
    file->ensure_page_exists(record.rid.page_no);
    if (file->is_record(record.rid)) {
        auto current = file->get_record(record.rid, nullptr);
        if (std::memcmp(current->data, desired.data(), desired.size()) != 0) {
            file->update_record(record.rid, const_cast<char *>(desired.data()), nullptr);
        }
    } else {
        file->insert_record(record.rid, const_cast<char *>(desired.data()));
    }
}

void RecoveryManager::redo() {
    for (const auto &record : records_) {
        if (committed_.count(record.txn_id) == 0) continue;
        if (record.type == LogType::UPDATE || record.type == LogType::INSERT || record.type == LogType::DELETE) {
            apply_record(record, true);
        }
    }
}

void RecoveryManager::undo() {
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
        if (committed_.count(it->txn_id) != 0 || aborted_.count(it->txn_id) != 0) continue;
        if (it->type == LogType::UPDATE || it->type == LogType::INSERT || it->type == LogType::DELETE) {
            apply_record(*it, false);
        }
    }
    sm_manager_->rebuild_all_indexes();
    sm_manager_->flush_all();
    disk_manager_->reset_log();
    records_.clear();
    committed_.clear();
    aborted_.clear();
}
