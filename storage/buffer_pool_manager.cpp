#include "buffer_pool_manager.h"

/**
 * @description: 从缓冲池中获取一个指定的页。
 * @return {Page*} 若成功获取页，则返回其指针；否则返回nullptr。
 * @param {PageId} page_id 要获取的页面的唯一标识。
 */
Page *BufferPoolManager::fetch_page(PageId page_id) {
    // 根据页面ID的哈希值确定要访问的缓冲池实例。
    int target_instance_index = get_instance_no(page_id);
    auto* target_instance = instances_[target_instance_index];
    
    // 调用对应实例的fetch_page方法来获取页面。
    Page* fetched_page = target_instance->fetch_page(page_id);
    
    // 返回获取到的页面指针。
    return fetched_page;
}

/**
 * @description: 解除固定（unpin）一个缓冲池中的页面。
 * @return {bool} 若页面的pin_count已经小于等于0，返回false；否则返回true。
 * @param {PageId} page_id 目标页面的ID。
 * @param {bool} is_dirty 如果页面被修改过，则为true；否则为false。
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    int instance_idx = get_instance_no(page_id);
    BufferPoolInstance* bpi_instance = instances_[instance_idx];
    
    // 执行unpin操作，并返回结果。
    return bpi_instance->unpin_page(page_id, is_dirty);
}

/**
 * @description: 将指定的页强制写回磁盘，忽略其是否正在使用。
 * @return {bool} 成功写回返回true；若页面不在页表中则返回false。
 * @param {PageId} page_id 要写回的页面的ID。
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    // 查找并调用正确的缓冲池实例来执行操作。
    return instances_[get_instance_no(page_id)]->flush_page(page_id);
}

/**
 * @description: 创建一个新的页面，并将其加载到缓冲池。
 * @return {Page*} 返回新创建的页面的指针，若失败则返回nullptr。
 * @param {PageId*} page_id 指向PageId的指针，用于存储新页面的ID。
 */
Page *BufferPoolManager::new_page(PageId *page_id) {
    // 首先在磁盘上为新页面分配空间并更新page_id。
    int allocated_page_no = disk_manager_->allocate_page(page_id->fd);
    PageId new_page_id = {page_id->fd, allocated_page_no};
    
    // 根据新页面的ID，找到对应的缓冲池实例。
    int target_instance_index = get_instance_no(new_page_id);
    
    // 调用实例的new_page方法来创建页面。
    Page* new_page_ptr = instances_[target_instance_index]->new_page(&new_page_id);
    
    // 将更新后的page_id赋值给传入的参数。
    *page_id = new_page_id;
    
    // 返回新页面的指针。
    return new_page_ptr;
}

/**
 * @description: 从缓冲池中删除指定的页面。
 * @return {bool} 如果页面不存在于缓冲池或成功被删除，返回true；如果存在但无法删除（例如pin_count > 0），则返回false。
 * @param {PageId} page_id 目标页面的ID。
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    // 根据页面ID找到对应的缓冲池实例并调用删除函数。
    return instances_[get_instance_no(page_id)]->delete_page(page_id);
}

/**
 * @description: 将指定文件句柄下的所有页面刷写回磁盘。
 * @param {int} fd 文件句柄。
 */
void BufferPoolManager::flush_all_pages(int fd) {
    // 遍历所有缓冲池实例，对每个实例执行刷写操作。
    for (auto& instance_ptr : instances_) {
        instance_ptr->flush_all_pages(fd);
    }
}

/**
 * @description: 为创建检查点而将指定文件句柄下的所有页面刷写回磁盘。
 * @param {int} fd 文件句柄。
 */
void BufferPoolManager::flush_all_pages_for_checkpoint(int fd) {
    for (auto& buffer_instance : instances_) {
        buffer_instance->flush_all_pages_for_checkpoint(fd);
    }
}

/**
 * @description: 删除指定文件句柄下的所有页面。
 * @param {int} fd 文件句柄。
 */
void BufferPoolManager::delete_all_pages(int fd) {
    // 遍历并对所有实例执行删除操作。
    for (auto const& buffer_instance : instances_) {
        buffer_instance->delete_all_pages(fd);
    }
}