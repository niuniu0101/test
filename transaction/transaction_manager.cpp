#include "transaction_manager.h"

#include <record/rm_manager.h>

#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction *TransactionManager::begin(Transaction *txn, LogManager *log_manager) {
    std::lock_guard lock(latch_);

    bool need_new_txn = txn == nullptr;
    if (need_new_txn) {
        txn_id_t new_id = next_txn_id_++;
        txn = new Transaction(new_id);
    }

    timestamp_t new_ts = next_timestamp_++;
    txn->set_start_ts(new_ts);

    txn_id_t current_txn_id = txn->get_transaction_id();
    txn_map.emplace(current_txn_id, txn);

#ifdef ENABLE_LOGGING
    auto *begin_log_record = new BeginLogRecord(txn->get_transaction_id());
    lsn_t prev_lsn = txn->get_prev_lsn();
    begin_log_record->prev_lsn_ = prev_lsn;
    
    // TODO 日志管理
    lsn_t new_lsn = log_manager->add_log_to_buffer(begin_log_record);
    txn->set_prev_lsn(new_lsn);
    
    delete begin_log_record;
#endif

    Transaction *result = txn;
    return result;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    std::lock_guard lock(latch_);

    auto write_set = txn->get_write_set();
    for (auto &write_record : *write_set) {
        delete write_record;
    }
    
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    lock_set->clear();

#ifdef ENABLE_LOGGING
    auto *commit_log_record = new CommitLogRecord(txn->get_transaction_id());
    lsn_t prev_lsn = txn->get_prev_lsn();
    commit_log_record->prev_lsn_ = prev_lsn;

    // TODO 日志管理
    lsn_t commit_lsn = log_manager->add_log_to_buffer(commit_log_record);
    txn->set_prev_lsn(commit_lsn);
    
    log_manager->flush_log_to_disk();
    
    delete commit_log_record;
#endif

    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    // Todo:
    // 1. 回滚所有写操作
    // 2. 释放所有锁
    // 3. 清空事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    std::lock_guard lock(latch_);

    auto &&write_set = txn->get_write_set();
    auto *context = new Context(lock_manager_, log_manager, txn);
    // 从最后一个向前回滚
    for (auto &&write_iter = write_set->rbegin(); write_iter != write_set->rend(); ++write_iter) {
        auto &write_record = *write_iter;
        auto &table_name = write_record->GetTableName();
        auto &table_meta = sm_manager_->db_.get_table(table_name);
        auto &fh = sm_manager_->fhs_[table_name];
        WType write_type = write_record->GetWriteType();
        switch (write_type) {
            case WType::INSERT_TUPLE: {
                // 删除记录
                auto &rid = write_record->GetRid();
                auto &record = write_record->GetRecord();
                fh->delete_record(rid, context);
                // 删除索引
                for (auto &index_entry: table_meta.indexes) {
                    auto &index_name = index_entry.first;
                    auto &index_meta = index_entry.second;
                    char *key = new char[index_meta.col_tot_len];
                    for (auto &col_entry: index_meta.cols) {
                        auto &index_offset = col_entry.first;
                        auto &col_meta = col_entry.second;
                        memcpy(key + index_offset, record.data + col_meta.offset, col_meta.len);
                    }
                    auto ih = sm_manager_->ihs_[index_name].get();
                    ih->rw_latch_.WLock();
                    ih->delete_entry(key, txn);
                    ih->rw_latch_.WUnlock();
                    delete []key;
                }
#ifdef ENABLE_LOGGING
                // 生成删除日志
                auto *delete_log_record = new DeleteLogRecord(txn->get_transaction_id(), record, rid, table_name);
                lsn_t prev_lsn = txn->get_prev_lsn();
                delete_log_record->prev_lsn_ = prev_lsn;
                lsn_t new_lsn = log_manager->add_log_to_buffer(delete_log_record);
                txn->set_prev_lsn(new_lsn);
                delete delete_log_record;
#endif
                break;
            }
            case WType::DELETE_TUPLE: {
                auto &record = write_record->GetRecord();
                auto &rid = write_record->GetRid();
                fh->insert_record(rid, record.data);
                // 插入索引
                for (auto &index_entry: table_meta.indexes) {
                    auto &index_name = index_entry.first;
                    auto &index_meta = index_entry.second;
                    char *key = new char[index_meta.col_tot_len];
                    for (auto &col_entry: index_meta.cols) {
                        auto &index_offset = col_entry.first;
                        auto &col_meta = col_entry.second;
                        memcpy(key + index_offset, record.data + col_meta.offset, col_meta.len);
                    }
                    auto ih = sm_manager_->ihs_[index_name].get();
                    ih->rw_latch_.WLock();
                    ih->insert_entry(key, rid, txn);
                    ih->rw_latch_.WUnlock();
                    delete []key;
                }
#ifdef ENABLE_LOGGING
                // 生成插入日志
                auto *insert_log_record = new InsertLogRecord(txn->get_transaction_id(), record, rid, table_name);
                lsn_t prev_lsn = txn->get_prev_lsn();
                insert_log_record->prev_lsn_ = prev_lsn;
                lsn_t new_lsn = log_manager->add_log_to_buffer(insert_log_record);
                txn->set_prev_lsn(new_lsn);
                delete insert_log_record;
#endif
                break;
            }
            case WType::UPDATE_TUPLE: {
                auto &old_record = write_record->GetRecord();
                auto &new_record = write_record->GetUpdatedRecord();
                auto &rid = write_record->GetRid();
                fh->update_record(rid, old_record.data, context);
                // 删除新索引，插入旧索引
                bool has_index_key = write_record->is_set_index_key();
                if (has_index_key) {
                    for (auto &index_entry: table_meta.indexes) {
                        auto &index_name = index_entry.first;
                        auto &index_meta = index_entry.second;
                        char *old_key = new char[index_meta.col_tot_len];
                        char *new_key = new char[index_meta.col_tot_len];
                        for (auto &col_entry: index_meta.cols) {
                            auto &index_offset = col_entry.first;
                            auto &col_meta = col_entry.second;
                            memcpy(old_key + index_offset, old_record.data + col_meta.offset, col_meta.len);
                            memcpy(new_key + index_offset, new_record.data + col_meta.offset, col_meta.len);
                        }
                        auto ih = sm_manager_->ihs_[index_name].get();
                        ih->rw_latch_.WLock();
                        ih->delete_entry(new_key, txn);
                        ih->insert_entry(old_key, rid, txn);
                        ih->rw_latch_.WUnlock();
                        delete []old_key;
                        delete []new_key;
                    }
                }
#ifdef ENABLE_LOGGING
                // 生成插入日志
                auto *update_log_record = new UpdateLogRecord(txn->get_transaction_id(), new_record, old_record, rid,
                                                              table_name);
                lsn_t prev_lsn = txn->get_prev_lsn();
                update_log_record->prev_lsn_ = prev_lsn;
                lsn_t new_lsn = log_manager->add_log_to_buffer(update_log_record);
                txn->set_prev_lsn(new_lsn);
                delete update_log_record;
#endif
                break;
            }
            default:
                throw InternalError("Unexpected WType！");
        }
        delete write_record;
    }
    delete context;
    write_set->clear();

    // 释放所有锁
    auto &&lock_set = txn->get_lock_set();
    for (auto &lock_data_id: *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    lock_set->clear();
#ifdef ENABLE_LOGGING
    auto *abort_log_record = new AbortLogRecord(txn->get_transaction_id());
    lsn_t prev_lsn = txn->get_prev_lsn();
    abort_log_record->prev_lsn_ = prev_lsn;
    // TODO 日志管理
    lsn_t new_lsn = log_manager->add_log_to_buffer(abort_log_record);
    txn->set_prev_lsn(new_lsn);
    log_manager->flush_log_to_disk();
    delete abort_log_record;
#endif
    txn->set_state(TransactionState::ABORTED);
}
