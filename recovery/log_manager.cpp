#include <cstring>
#include "log_manager.h"

/**
 * @description: 将一个日志记录添加到内部日志缓冲区，并为其分配一个全局唯一的日志序列号（LSN）。
 * @param {LogRecord*} log_record 指向要写入缓冲区的日志记录的指针。
 * @return {lsn_t} 返回分配给该日志记录的日志序列号。
 */
lsn_t LogManager::add_log_to_buffer(LogRecord *log_record) {
    latch_.lock();

    // 检查当前日志缓冲区是否有足够的空间容纳新的日志记录。
    // 如果缓冲区已满，则需要先将缓冲区内容刷写到磁盘。
    if (log_buffer_.is_full(log_record->log_tot_len_)) {
        latch_.unlock(); // 暂时释放锁，允许其他线程在刷写时继续进行
        flush_log_to_disk();
        latch_.lock(); // 重新获取锁以继续操作
    }

    // 分配并设置日志序列号（LSN）。
    lsn_t current_lsn_val = global_lsn_++;
    log_record->lsn_ = current_lsn_val;

    // 将日志记录的数据序列化并复制到缓冲区中。
    char* buffer_write_position = log_buffer_.buffer_ + log_buffer_.offset_;
    log_record->serialize(buffer_write_position);

    // 更新缓冲区的写入偏移量。
    log_buffer_.offset_ += log_record->log_tot_len_;

    latch_.unlock();

    return log_record->lsn_;
}

/**
 * @description: 将日志缓冲区中的所有内容持久化到磁盘。此操作会阻塞所有其他日志操作。
 */
void LogManager::flush_log_to_disk() {
    std::lock_guard exclusive_lock(latch_);

    // 只有当缓冲区中有数据时才执行刷写操作。
    size_t current_buffer_offset = log_buffer_.offset_;
    if (current_buffer_offset == 0) {
        return;
    }

    // 调用磁盘管理器进行写日志操作。
    disk_manager_->write_log(log_buffer_.buffer_, current_buffer_offset);

    // 刷写成功后，重置缓冲区偏移量并更新持久化 LSN。
    log_buffer_.offset_ = 0;
    persist_lsn_ = global_lsn_ - 1;
}