#include "log_recovery.h"
#include <queue>
#include <algorithm> // 引入算法头文件
#include "transaction/transaction_manager.h"

/**
 * @description: 在故障恢复的分析（Analyze）阶段，重建脏页表（DPT）和未完成事务列表（ATT）。
 */
void RecoveryManager::analyze() {
    lsn_t latest_lsn = INVALID_LSN;
    txn_id_t latest_txn_id = INVALID_TXN_ID;
    std::size_t current_log_offset = 0;
    std::size_t bytes_read_from_disk;

    // 循环读取日志文件，直到没有更多日志可读。
    while ((bytes_read_from_disk = disk_manager_->read_log(buffer_.buffer_, LOG_BUFFER_SIZE, current_log_offset)) > 0) {
        buffer_.offset_ = 0;
        
        // 遍历当前读取的日志缓冲区。
        while (buffer_.offset_ + LOG_HEADER_SIZE <= bytes_read_from_disk) {
            // 获取当前日志记录的总长度。
            const uint32_t log_length = *reinterpret_cast<const uint32_t*>(buffer_.buffer_ + buffer_.offset_ + OFFSET_LOG_TOT_LEN);
            
            // 如果日志记录不完整，则跳出内层循环，等待下一次从磁盘读取更多数据。
            if (log_length > bytes_read_from_disk - buffer_.offset_) {
                break;
            }

            // 解析日志类型。
            const LogType record_type = *reinterpret_cast<const LogType*>(buffer_.buffer_ + buffer_.offset_ + OFFSET_LOG_TYPE);
            
            LogRecord* current_log_record = nullptr;
            
            // 根据日志类型进行处理。
            switch (record_type) {
                case BEGIN: {
                    auto* begin_log = new BeginLogRecord();
                    begin_log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    active_txn_.emplace(begin_log->log_tid_, begin_log->lsn_);
                    current_log_record = begin_log;
                    break;
                }
                case COMMIT: {
                    auto* commit_log = new CommitLogRecord();
                    commit_log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    active_txn_.erase(commit_log->log_tid_);
                    current_log_record = commit_log;
                    break;
                }
                case ABORT: {
                    auto* abort_log = new AbortLogRecord();
                    abort_log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    active_txn_.erase(abort_log->log_tid_);
                    current_log_record = abort_log;
                    break;
                }
                case INSERT: {
                    auto* insert_log = new InsertLogRecord();
                    insert_log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    active_txn_[insert_log->log_tid_] = insert_log->lsn_;
                    
                    auto file_handle = sm_manager_->fhs_.at(insert_log->table_name_).get();
                    try {
                        file_handle->fetch_page_handle(insert_log->rid_.page_no);
                    } catch (RMDBError &e) {
                        file_handle->create_new_page_handle();
                    }
                    auto page_handle = file_handle->fetch_page_handle(insert_log->rid_.page_no);
                    
                    if (page_handle.page->get_page_lsn() < insert_log->lsn_) {
                        dirty_page_table_.emplace_back(insert_log->lsn_);
                        page_handle.page->set_page_lsn(insert_log->lsn_);
                    }
                    
                    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
                    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);

                    current_log_record = insert_log;
                    break;
                }
                case DELETE: {
                    auto* delete_log = new DeleteLogRecord();
                    delete_log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    active_txn_[delete_log->log_tid_] = delete_log->lsn_;
                    
                    auto file_handle = sm_manager_->fhs_.at(delete_log->table_name_).get();
                    try {
                        file_handle->fetch_page_handle(delete_log->rid_.page_no);
                    } catch (RMDBError &e) {
                        file_handle->create_new_page_handle();
                    }
                    auto page_handle = file_handle->fetch_page_handle(delete_log->rid_.page_no);
                    
                    if (page_handle.page->get_page_lsn() < delete_log->lsn_) {
                        dirty_page_table_.emplace_back(delete_log->lsn_);
                        page_handle.page->set_page_lsn(delete_log->lsn_);
                    }
                    
                    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
                    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);

                    current_log_record = delete_log;
                    break;
                }
                case UPDATE: {
                    auto* update_log = new UpdateLogRecord();
                    update_log->deserialize(buffer_.buffer_ + buffer_.offset_);
                    active_txn_[update_log->log_tid_] = update_log->lsn_;

                    auto file_handle = sm_manager_->fhs_.at(update_log->table_name_).get();
                    try {
                        file_handle->fetch_page_handle(update_log->rid_.page_no);
                    } catch (RMDBError &e) {
                        file_handle->create_new_page_handle();
                    }
                    auto page_handle = file_handle->fetch_page_handle(update_log->rid_.page_no);
                    
                    if (page_handle.page->get_page_lsn() < update_log->lsn_) {
                        dirty_page_table_.emplace_back(update_log->lsn_);
                        page_handle.page->set_page_lsn(update_log->lsn_);
                    }
                    
                    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
                    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);

                    current_log_record = update_log;
                    break;
                }
                default: {
                    // 对于未处理的日志类型，跳过。
                    break;
                }
            }

            // 更新日志映射表、LSN和TxnID。
            if (current_log_record) {
                lsn_mapping_.emplace(current_log_record->lsn_, current_log_offset + buffer_.offset_);
                latest_lsn = std::max(latest_lsn, current_log_record->lsn_);
                latest_txn_id = std::max(latest_txn_id, current_log_record->log_tid_);
                buffer_.offset_ += current_log_record->log_tot_len_;
                delete current_log_record;
            } else {
                 buffer_.offset_ += log_length;
            }
        }
        
        // 更新总偏移量并重置缓冲区。
        current_log_offset += buffer_.offset_;
        std::memset(buffer_.buffer_, 0, LOG_BUFFER_SIZE);
    }
    
    // 设置日志管理器和事务管理器的状态。
    log_manager_->set_global_lsn(latest_lsn + 1);
    log_manager_->set_persist_lsn(latest_lsn);
    transaction_manager_->set_next_txn_id(latest_txn_id + 1);
}

/**
 * @description: 在恢复（Redo）阶段，重做所有未落盘的操作。
 */
void RecoveryManager::redo() {
    // 遍历脏页表，按LSN顺序执行重做操作。
    for (const auto& lsn_to_redo : dirty_page_table_) {
        int log_file_offset = lsn_mapping_[lsn_to_redo];
        disk_manager_->read_log(buffer_.buffer_, LOG_BUFFER_SIZE, log_file_offset);
        
        const LogType log_entry_type = *reinterpret_cast<const LogType*>(buffer_.buffer_ + OFFSET_LOG_TYPE);
        
        switch (log_entry_type) {
            case INSERT: {
                auto* insert_log_entry = new InsertLogRecord();
                insert_log_entry->deserialize(buffer_.buffer_);
                
                auto file_handler = sm_manager_->fhs_.at(insert_log_entry->table_name_).get();
                file_handler->insert_record(insert_log_entry->rid_, insert_log_entry->insert_value_.data);

                auto& table_indexes = sm_manager_->db_.get_table(insert_log_entry->table_name_).indexes;
                for (auto const& [index_name, index_meta] : table_indexes) {
                    char* key_buffer = new char[index_meta.col_tot_len];
                    auto& index_handler = sm_manager_->ihs_.at(index_name);
                    for (const auto& [col_offset, column_meta] : index_meta.cols) {
                        std::memcpy(key_buffer + col_offset, insert_log_entry->insert_value_.data + column_meta.offset, column_meta.len);
                    }
                    index_handler->insert_entry(key_buffer, insert_log_entry->rid_, &transaction_);
                    delete[] key_buffer;
                }
                
                delete insert_log_entry;
                break;
            }
            case DELETE: {
                auto* delete_log_entry = new DeleteLogRecord();
                delete_log_entry->deserialize(buffer_.buffer_);
                
                auto file_handler = sm_manager_->fhs_.at(delete_log_entry->table_name_).get();
                try {
                    file_handler->delete_record(delete_log_entry->rid_, nullptr);
                } catch (RecordNotFoundError &e) {
                    // 如果记录已经被删除，忽略该错误。
                }

                auto& table_indexes = sm_manager_->db_.get_table(delete_log_entry->table_name_).indexes;
                for (auto const& [index_name, index_meta] : table_indexes) {
                    char* key_buffer = new char[index_meta.col_tot_len];
                    auto& index_handler = sm_manager_->ihs_.at(index_name);
                    for (const auto& [col_offset, column_meta] : index_meta.cols) {
                        std::memcpy(key_buffer + col_offset, delete_log_entry->delete_value_.data + column_meta.offset, column_meta.len);
                    }
                    index_handler->delete_entry(key_buffer, &transaction_);
                    delete[] key_buffer;
                }

                delete delete_log_entry;
                break;
            }
            case UPDATE: {
                auto* update_log_entry = new UpdateLogRecord();
                update_log_entry->deserialize(buffer_.buffer_);
                
                auto file_handler = sm_manager_->fhs_.at(update_log_entry->table_name_).get();
                file_handler->update_record(update_log_entry->rid_, update_log_entry->update_value_.data, nullptr);

                auto& table_indexes = sm_manager_->db_.get_table(update_log_entry->table_name_).indexes;
                for (auto const& [index_name, index_meta] : table_indexes) {
                    char* old_key_buffer = new char[index_meta.col_tot_len];
                    char* new_key_buffer = new char[index_meta.col_tot_len];
                    auto& index_handler = sm_manager_->ihs_.at(index_name);
                    for (const auto& [col_offset, column_meta] : index_meta.cols) {
                        std::memcpy(old_key_buffer + col_offset, update_log_entry->old_value_.data + column_meta.offset, column_meta.len);
                        std::memcpy(new_key_buffer + col_offset, update_log_entry->update_value_.data + column_meta.offset, column_meta.len);
                    }
                    index_handler->delete_entry(old_key_buffer, &transaction_);
                    index_handler->insert_entry(new_key_buffer, update_log_entry->rid_, &transaction_);
                    delete[] old_key_buffer;
                    delete[] new_key_buffer;
                }

                delete update_log_entry;
                break;
            }
            default:
                break;
        }
    }
}

/**
 * @description: 在撤销（Undo）阶段，回滚所有未完成的事务。
 */
void RecoveryManager::undo() {
    std::priority_queue<lsn_t> lsn_priority_queue;

    // 将所有活跃事务的最新LSN加入优先队列。
    for (const auto& [txn_id, lsn] : active_txn_) {
        lsn_priority_queue.emplace(lsn);
    }
    
    lsn_t current_lsn_to_undo;
    while (!lsn_priority_queue.empty()) {
        current_lsn_to_undo = lsn_priority_queue.top();
        lsn_priority_queue.pop();
        
        int log_file_offset = lsn_mapping_.at(current_lsn_to_undo);
        disk_manager_->read_log(buffer_.buffer_, LOG_BUFFER_SIZE, log_file_offset);
        
        const LogType log_entry_type = *reinterpret_cast<const LogType*>(buffer_.buffer_ + OFFSET_LOG_TYPE);
        
        lsn_t previous_lsn = INVALID_LSN;
        LogRecord* log_entry_to_process = nullptr;
        
        switch (log_entry_type) {
            case BEGIN: {
                auto* begin_log = new BeginLogRecord();
                begin_log->deserialize(buffer_.buffer_);
                previous_lsn = begin_log->prev_lsn_;
                log_entry_to_process = begin_log;
                break;
            }
            case COMMIT: {
                auto* commit_log = new CommitLogRecord();
                commit_log->deserialize(buffer_.buffer_);
                previous_lsn = commit_log->prev_lsn_;
                log_entry_to_process = commit_log;
                break;
            }
            case ABORT: {
                auto* abort_log = new AbortLogRecord();
                abort_log->deserialize(buffer_.buffer_);
                previous_lsn = abort_log->prev_lsn_;
                log_entry_to_process = abort_log;
                break;
            }
            case INSERT: {
                auto* insert_log = new InsertLogRecord();
                insert_log->deserialize(buffer_.buffer_);
                
                auto file_handle = sm_manager_->fhs_.at(insert_log->table_name_).get();
                file_handle->delete_record(insert_log->rid_, nullptr);

                auto& table_indexes = sm_manager_->db_.get_table(insert_log->table_name_).indexes;
                for (auto const& [index_name, index_meta] : table_indexes) {
                    char* key_buffer = new char[index_meta.col_tot_len];
                    auto& index_handle = sm_manager_->ihs_.at(index_name);
                    for (const auto& [col_offset, column_meta] : index_meta.cols) {
                        std::memcpy(key_buffer + col_offset, insert_log->insert_value_.data + column_meta.offset, column_meta.len);
                    }
                    index_handle->delete_entry(key_buffer, &transaction_);
                    delete[] key_buffer;
                }
                
                previous_lsn = insert_log->prev_lsn_;
                log_entry_to_process = insert_log;
                break;
            }
            case DELETE: {
                auto* delete_log = new DeleteLogRecord();
                delete_log->deserialize(buffer_.buffer_);

                auto file_handle = sm_manager_->fhs_.at(delete_log->table_name_).get();
                file_handle->insert_record(delete_log->rid_, delete_log->delete_value_.data);

                auto& table_indexes = sm_manager_->db_.get_table(delete_log->table_name_).indexes;
                for (auto const& [index_name, index_meta] : table_indexes) {
                    char* key_buffer = new char[index_meta.col_tot_len];
                    auto& index_handle = sm_manager_->ihs_.at(index_name);
                    for (const auto& [col_offset, column_meta] : index_meta.cols) {
                        std::memcpy(key_buffer + col_offset, delete_log->delete_value_.data + column_meta.offset, column_meta.len);
                    }
                    index_handle->insert_entry(key_buffer, delete_log->rid_, &transaction_);
                    delete[] key_buffer;
                }
                
                previous_lsn = delete_log->prev_lsn_;
                log_entry_to_process = delete_log;
                break;
            }
            case UPDATE: {
                auto* update_log = new UpdateLogRecord();
                update_log->deserialize(buffer_.buffer_);
                
                auto file_handle = sm_manager_->fhs_.at(update_log->table_name_).get();
                file_handle->update_record(update_log->rid_, update_log->old_value_.data, nullptr);

                auto& table_indexes = sm_manager_->db_.get_table(update_log->table_name_).indexes;
                for (auto const& [index_name, index_meta] : table_indexes) {
                    char* old_key_buffer = new char[index_meta.col_tot_len];
                    char* new_key_buffer = new char[index_meta.col_tot_len];
                    auto& index_handle = sm_manager_->ihs_.at(index_name);
                    for (const auto& [col_offset, column_meta] : index_meta.cols) {
                        std::memcpy(old_key_buffer + col_offset, update_log->old_value_.data + column_meta.offset, column_meta.len);
                        std::memcpy(new_key_buffer + col_offset, update_log->update_value_.data + column_meta.offset, column_meta.len);
                    }
                    index_handle->delete_entry(new_key_buffer, &transaction_);
                    index_handle->insert_entry(old_key_buffer, update_log->rid_, &transaction_);
                    delete[] old_key_buffer;
                    delete[] new_key_buffer;
                }

                previous_lsn = update_log->prev_lsn_;
                log_entry_to_process = update_log;
                break;
            }
            default:
                break;
        }

        if (log_entry_to_process) {
             delete log_entry_to_process;
        }

        if (previous_lsn != INVALID_LSN) {
            lsn_priority_queue.emplace(previous_lsn);
        }
    }
}

/**
 * @description: 在恢复阶段，重做所有受影响的表索引。
 */
void RecoveryManager::redo_indexes() {
    std::vector<std::string> column_names;
    auto* recovery_context = new Context(nullptr, nullptr, &transaction_);

    // 遍历所有表，并重做其索引。
    for (auto const& [table_name, _] : sm_manager_->fhs_) {
        auto& table_metadata = sm_manager_->db_.get_table(table_name);
        for (auto const& [index_name, index_meta] : table_metadata.indexes) {
            for (auto const& [_, col] : index_meta.cols) {
                column_names.emplace_back(col.name);
            }
            sm_manager_->redo_index(table_name, table_metadata, column_names, index_name, recovery_context);
            column_names.clear();
        }
    }
    
    delete recovery_context;
}