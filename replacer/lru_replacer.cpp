#include "lru_replacer.h"
#include <unistd.h>
#include <mutex> // 包含互斥量头文件，以确保线程安全

LRUReplacer::LRUReplacer(size_t num_pages) {
    max_size_ = num_pages;
}

LRUReplacer::~LRUReplacer() = default;

/**
 * @description: 使用LRU策略选择一个牺牲帧（victim frame），并返回其ID。
 * @param {frame_id_t*} frame_id 指向存储牺牲帧ID的指针。
 * @return {bool} 如果成功找到并移除了一个牺牲帧，返回true；否则返回false。
 */
bool LRUReplacer::victim(frame_id_t *frame_id) {
    std::scoped_lock exclusive_lock{latch_};

    // 检查LRU列表是否为空，若为空则无牺牲帧可供选择。
    bool is_empty = LRUlist_.empty();
    if (is_empty) {
        return false;
    }

    // 从LRU列表的末尾（最不常用）获取帧ID。
    frame_id_t victim_id = LRUlist_.back();
    *frame_id = victim_id;

    // 从LRU列表中移除该帧。
    LRUlist_.pop_back();

    // 从哈希表中移除该帧的映射。
    LRUhash_.erase(victim_id);

    return true;
}

/**
 * @description: 固定一个指定的帧，使其不能被淘汰。
 * @param {frame_id_t} frame_id 需要固定的帧的ID。
 */
void LRUReplacer::pin(frame_id_t frame_id) {
    std::scoped_lock exclusive_lock{latch_};

    // 在哈希表中查找目标帧。
    auto find_result = LRUhash_.find(frame_id);

    // 如果找到该帧，则将其从LRU数据结构中移除。
    if (find_result != LRUhash_.end()) {
        auto list_iterator = find_result->second;
        LRUlist_.erase(list_iterator);
        LRUhash_.erase(find_result);
    }
}

/**
 * @description: 取消固定一个帧，允许其再次参与淘汰。
 * @param {frame_id_t} frame_id 需要取消固定的帧的ID。
 */
void LRUReplacer::unpin(frame_id_t frame_id) {
    std::scoped_lock lock_guard{latch_};
    
    // 如果LRU列表已满，则不进行任何操作。
    size_t current_size = LRUlist_.size();
    if (current_size >= max_size_) {
        return;
    }
    
    // 如果帧ID已存在于哈希表中，则无需再次取消固定。
    if (LRUhash_.find(frame_id) != LRUhash_.end()) {
        return;
    }

    // 将新的帧添加到LRU列表的头部（最近使用）。
    LRUlist_.emplace_front(frame_id);
    
    // 在哈希表中创建新的映射。
    auto new_iterator = LRUlist_.begin();
    LRUhash_[frame_id] = new_iterator;
}

/**
 * @description: 获取当前可被淘汰的页面数量。
 * @return {size_t} 可被淘汰的页面数量。
 */
size_t LRUReplacer::Size() {
    std::scoped_lock guard_lock(latch_);
    return LRUlist_.size();
}