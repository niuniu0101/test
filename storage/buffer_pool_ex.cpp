#include "buffer_pool_ex.h"
#include "recovery/log_manager.h"

/**
 * @description: 解除固定（unpin）一个在缓冲池中的页。
 * @return {bool} 如果页不存在或pin_count已为0，返回false；成功unpin则返回true。
 * @param {PageId} page_id 目标页的PageId。
 * @param {bool} is_dirty 如果该页有修改，则为true。
 */
bool BufferPoolInstance::unpin_page(PageId page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(latch_);
    
    auto page_entry_it = page_table_.find(page_id);
    // 如果页不在页表中，则无法unpin。
    if (page_entry_it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_index = page_entry_it->second;
    Page* target_page = &pages_[frame_index];

    // 如果pin_count已经为0，则无法unpin。
    if (target_page->pin_count_ == 0) {
        return false;
    }

    // 减少pin_count。
    int temp_pin_count = --target_page->pin_count_;
    if (temp_pin_count == 0) {
        replacer_->unpin(frame_index);
    }
    
    // 根据参数设置脏页标志。
    if (is_dirty) {
        target_page->is_dirty_ = true;
    }
    
    return true;
}

/**
 * @description: 从free_list或replacer中得到可淘汰帧页的 *frame_id。
 * @return {bool} 若成功找到可替换帧，则返回true；否则返回false。
 * @param {frame_id_t*} frame_id 指向帧页id的指针，用于存储找到的可替换帧的id。
 */
bool BufferPoolInstance::find_victim_page(frame_id_t *frame_id) {
    bool victim_found = false;
    
    // 首先检查缓冲池中是否有空闲的帧。
    if (!free_list_.empty()) {
        frame_id_t temp_frame_id = free_list_.front();
        free_list_.pop_front();
        *frame_id = temp_frame_id;
        victim_found = true;
    } else {
        // 如果缓冲池已满，则通过淘汰策略选择一个牺牲帧。
        victim_found = replacer_->victim(frame_id);
    }
    
    return victim_found;
}

/**
 * @description: 将指定文件句柄下的所有脏页写回磁盘。
 * @param {int} fd 要刷写的文件的句柄。
 */
void BufferPoolInstance::flush_all_pages(int fd) {
    std::lock_guard<std::mutex> lock(latch_);

    // 遍历页表，找到匹配的文件句柄，并刷写对应的页。
    for (const auto& entry : page_table_) {
        const PageId& current_page_id = entry.first;
        frame_id_t current_frame_id = entry.second;
        
        if (current_page_id.fd == fd && current_frame_id != INVALID_FRAME_ID) {
            Page& current_page = pages_[current_frame_id];

#ifdef ENABLE_LOGGING
            if (log_manager_ && current_page.get_page_lsn() > log_manager_->get_persist_lsn()) {
                log_manager_->flush_log_to_disk();
            }
#endif
            
            disk_manager_->write_page(current_page.id_.fd, current_page.id_.page_no, current_page.data_, PAGE_SIZE);
            current_page.is_dirty_ = false;
        }
    }
}

/**
 * @description: 更新页面数据。如果页面是脏页，则将其写入磁盘，然后更新为新的页面。
 * 同时更新页面的元数据（数据、脏页标志、页面ID）和页表。
 * @param {Page*} page 指向要更新的页面的指针。
 * @param {PageId} new_page_id 新页面的PageId。
 * @param {frame_id_t} new_frame_id 新页面的帧ID。
 */
void BufferPoolInstance::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    // 如果页面是脏页，需要将其内容持久化到磁盘。
    if (page->is_dirty()) {
#ifdef ENABLE_LOGGING
        // 如果有日志管理器且页面的LSN大于持久化LSN，则需要先刷写日志。
        if (log_manager_ && page->get_page_lsn() > log_manager_->get_persist_lsn()) {
            log_manager_->flush_log_to_disk();
        }
#endif
        // 将脏页内容写入磁盘。
        disk_manager_->write_page(page->get_page_id().fd, page->get_page_id().page_no, page->get_data(), PAGE_SIZE);
        page->is_dirty_ = false;
    }

    // 从页表中删除旧的页面映射，并建立新的映射。
    page_table_.erase(page->get_page_id());
    page_table_[new_page_id] = new_frame_id;

    // 重置页面内存并更新其元数据。
    page->reset_memory();
    page->id_ = new_page_id;
    page->pin_count_ = 0;
}

/**
 * @description: 删除指定文件句柄下的所有页。
 * @param {int} fd 要删除页面的文件句柄。
 */
void BufferPoolInstance::delete_all_pages(int fd) {
    std::lock_guard<std::mutex> lock(latch_);

    // 遍历并删除匹配的页。
    for (auto it = page_table_.begin(); it != page_table_.end();) {
        const PageId& current_page_id = it->first;
        if (current_page_id.fd == fd && it->second != INVALID_FRAME_ID) {
            // 获取帧ID。
            frame_id_t current_frame_id = it->second;
            Page& current_page = pages_[current_frame_id];

            // 重置页面的所有状态。
            current_page.reset_memory();
            current_page.is_dirty_ = false;
            current_page.pin_count_ = 0;
            current_page.id_.page_no = INVALID_PAGE_ID;

            // 将帧返回给空闲列表。
            free_list_.push_back(current_frame_id);
            // 从页表中移除。
            it = page_table_.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * @description: 从缓冲池中获取一个页。如果页已存在，则增加其pin_count。
 * 如果页不在缓冲池中，则从磁盘读取，并替换一个牺牲页。
 * @return {Page*} 返回获取到的页面的指针，若失败则返回nullptr。
 * @param {PageId} page_id 要获取的页面的PageId。
 */
Page *BufferPoolInstance::fetch_page(PageId page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    
    auto iter = page_table_.find(page_id);
    // 检查页是否已经在缓冲池中。
    if (iter != page_table_.end()) {
        frame_id_t current_frame_id = iter->second;
        Page* current_page = &pages_[current_frame_id];

        // 增加pin_count。如果pin_count从0变为1，则需要通知替换器该页被固定。
        if (++current_page->pin_count_ == 1) {
            replacer_->pin(current_frame_id);
        }
        
        return current_page;
    }

    // 页不在缓冲池中，需要从磁盘加载。
    frame_id_t available_frame_id = INVALID_FRAME_ID;
    
    // 尝试找到一个可以替换的帧。
    if (!find_victim_page(&available_frame_id)) {
        return nullptr; // 没有可替换的帧，返回失败。
    }

    // 更新牺牲页并加载新页。
    Page* target_page = &pages_[available_frame_id];
    update_page(target_page, page_id, available_frame_id);
    disk_manager_->read_page(page_id.fd, page_id.page_no, target_page->get_data(), PAGE_SIZE);

    // 固定新加载的页。
    replacer_->pin(available_frame_id);
    target_page->pin_count_ = 1;
    
    return target_page;
}

/**
 * @description: 为创建检查点而将指定文件句柄下的所有页写回磁盘。
 * @param {int} fd 文件句柄。
 */
void BufferPoolInstance::flush_all_pages_for_checkpoint(int fd) {
    std::lock_guard<std::mutex> lock(latch_);

    for (auto const& [page_id, frame_id] : page_table_) {
        if (page_id.fd == fd) {
            Page& page_ref = pages_[frame_id];
            
            // LSN重置为初始状态。
            page_ref.set_page_lsn(INVALID_LSN);
            disk_manager_->write_page(page_ref.id_.fd, page_ref.id_.page_no, page_ref.data_, PAGE_SIZE);
            page_ref.is_dirty_ = false;
        }
    }
}

/**
 * @description: 将目标页的内容强制写回磁盘。
 * @return {bool} 成功写回则返回true，如果页不存在则返回false。
 * @param {PageId} page_id 目标页的PageId。
 */
bool BufferPoolInstance::flush_page(PageId page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    auto search_result = page_table_.find(page_id);
    // 检查页是否存在于缓冲池中。
    if (search_result == page_table_.end()) {
        return false;
    }
    
    frame_id_t page_frame_id = search_result->second;
    Page& page_to_flush = pages_[page_frame_id];

#ifdef ENABLE_LOGGING
    // 在写回前，如果需要，先刷写日志。
    if (log_manager_ && page_to_flush.get_page_lsn() > log_manager_->get_persist_lsn()) {
        log_manager_->flush_log_to_disk();
    }
#endif
    
    // 执行写回操作并清除脏页标志。
    disk_manager_->write_page(page_to_flush.id_.fd, page_to_flush.id_.page_no, page_to_flush.data_, PAGE_SIZE);
    page_to_flush.is_dirty_ = false;
    
    return true;
}

/**
 * @description: 创建一个新页，并将其加载到缓冲池。
 * @return {Page*} 返回新创建的页面的指针，若失败则返回nullptr。
 * @param {PageId*} page_id 指向PageId的指针，用于返回新页面的PageId。
 */
Page *BufferPoolInstance::new_page(PageId *page_id) {
    std::lock_guard<std::mutex> lock(latch_);
    
    frame_id_t victim_frame_id = -1;
    // 查找一个可用的帧。
    if (!find_victim_page(&victim_frame_id)) {
        return nullptr; // 没有可用的帧，创建失败。
    }
    
    // 更新牺牲页为新页。
    Page* new_page_ptr = &pages_[victim_frame_id];
    update_page(new_page_ptr, *page_id, victim_frame_id);

    // 固定新页并设置其pin_count。
    replacer_->pin(victim_frame_id);
    new_page_ptr->pin_count_ = 1;
    
    return new_page_ptr;
}

/**
 * @description: 从缓冲池中删除一个页。
 * @return {bool} 如果页不存在或成功删除则返回true；如果页正在使用中，则返回false。
 * @param {PageId} page_id 目标页的PageId。
 */
bool BufferPoolInstance::delete_page(PageId page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    auto page_map_entry = page_table_.find(page_id);
    if (page_map_entry == page_table_.end()) {
        // 页不存在，视为成功。
        return true;
    }

    frame_id_t frame_index = page_map_entry->second;
    Page& page_to_delete = pages_[frame_index];

    // 正在使用的页不能被删除。
    if (page_to_delete.pin_count_ > 0) {
        return false;
    }

    // 如果是脏页，先写回磁盘。
    if (page_to_delete.is_dirty_) {
#ifdef ENABLE_LOGGING
        if (log_manager_ && page_to_delete.get_page_lsn() > log_manager_->get_persist_lsn()) {
            log_manager_->flush_log_to_disk();
        }
#endif
        disk_manager_->write_page(page_to_delete.id_.fd, page_to_delete.id_.page_no, page_to_delete.data_, PAGE_SIZE);
        page_to_delete.is_dirty_ = false;
    }

    // 将帧归还到空闲列表，从页表中移除该页，并重置页面的元数据。
    free_list_.push_back(frame_index);
    page_table_.erase(page_map_entry);

    page_to_delete.reset_memory();
    
    return true;
}