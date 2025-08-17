#include "rm_file_handle.h"

/**
 * @description: 获取指定记录号`rec_id`的记录
 * @param {Rid&} rec_id 记录号，用于定位记录位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} 指向`rec_id`对应记录对象的智能指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid &rec_id, Context *context) const {
    // 步骤：
    // 1. 获取包含指定记录的页面句柄
    // 2. 检查记录是否有效
    // 3. 构建并返回RmRecord对象
    // 读记录操作通常由表锁或间隙锁保护，无需额外的行级S锁。
    auto &&page_container = fetch_page_handle(rec_id.page_no);
    if (!Bitmap::is_set(page_container.bitmap, rec_id.slot_no)) {
        throw RecordNotFoundError(rec_id.page_no, rec_id.slot_no);
    }
    auto current_record = std::make_unique<RmRecord>(page_container.get_slot(rec_id.slot_no), file_hdr_.record_size, true);
    buffer_pool_manager_->unpin_page(page_container.page->get_page_id(), false);
    return std::move(current_record);
}

/**
 * @description: 在当前文件中插入一条记录，不指定插入位置
 * @param {char*} record_data_ptr 要插入的记录数据
 * @param {Context*} context
 * @return {Rid} 插入记录的记录号
 */
Rid RmFileHandle::insert_record(char *record_data_ptr, Context *context) {
    // 步骤：
    // 1. 获取一个有空闲空间的页面句柄
    // 2. 在该页面中找到第一个空闲槽位
    // 3. 将数据复制到该槽位
    // 4. 更新页面头部信息，如果页面已满，则更新文件头部信息
    auto &&page_container = create_page_handle();
    page_container.page->WLatch();
    auto &&free_slot_no = Bitmap::first_bit(false, page_container.bitmap, file_hdr_.num_records_per_page);
    memcpy(page_container.get_slot(free_slot_no), record_data_ptr, file_hdr_.record_size);
    Bitmap::set(page_container.bitmap, free_slot_no);

    if (++page_container.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_container.page_hdr->next_free_page_no;
    }

    Rid inserted_rec_id{page_container.page->get_page_id().page_no, free_slot_no};
    page_container.page->WUnlatch();
    buffer_pool_manager_->unpin_page(page_container.page->get_page_id(), true);
    return inserted_rec_id;
}

/**
 * @description: 在指定位置插入一条记录
 * @param {Rid&} rec_id 要插入记录的位置
 * @param {char*} record_data_ptr 要插入的记录数据
 */
void RmFileHandle::insert_record(const Rid &rec_id, char *record_data_ptr) {
    auto &&page_container = fetch_page_handle(rec_id.page_no);
    page_container.page->WLatch();
    if (!Bitmap::is_set(page_container.bitmap, rec_id.slot_no)) {
        Bitmap::set(page_container.bitmap, rec_id.slot_no);
        if (++page_container.page_hdr->num_records == file_hdr_.num_records_per_page) {
            file_hdr_.first_free_page_no = page_container.page_hdr->next_free_page_no;
        }
    }
    memcpy(page_container.get_slot(rec_id.slot_no), record_data_ptr, file_hdr_.record_size);
    page_container.page->WUnlatch();
    buffer_pool_manager_->unpin_page(page_container.page->get_page_id(), true);
}

/** 为 bulk load 载入数据，直接找到插入位置，不进行检查
 * @description: 在指定页面载入记录数据
 * @param {int&} page_no 页面号
 * @param {char*&} data_ptr 记录数据指针
 * @param {int} nums_record 记录数量
 * @param {int} page_size 页面大小
 */
void RmFileHandle::load_record(int &page_no, char *&data_ptr, int nums_record, int page_size) {
    PageId page_id{fd_, page_no};
    auto &&page_container = RmPageHandle{&file_hdr_, buffer_pool_manager_->new_page(&page_id)};
    assert(page_id.page_no == page_no);
    memcpy(page_container.slots, data_ptr, page_size);
    for (int i = 0; i < nums_record; ++i) {
        Bitmap::set(page_container.bitmap, i);
    }
    page_container.page_hdr->num_records += nums_record;
    page_container.page_hdr->next_free_page_no = INVALID_PAGE_ID;
    ++file_hdr_.num_pages;
    buffer_pool_manager_->unpin_page(page_container.page->get_page_id(), true);
}

/**
 * @description: 删除指定记录号`rec_id`的记录
 * @param {Rid&} rec_id 要删除的记录号
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid &rec_id, Context *context) {
    // 步骤：
    // 1. 获取指定记录所在的页面句柄
    // 2. 检查记录是否存在
    // 3. 更新页面的位图和记录计数
    // 4. 如果页面因此变空，则更新文件头部的空闲页链表
    // 该操作有间隙锁保护，无需行级X锁
    auto &&page_container = fetch_page_handle(rec_id.page_no);
    page_container.page->WLatch();
    if (!Bitmap::is_set(page_container.bitmap, rec_id.slot_no)) {
        throw RecordNotFoundError(rec_id.page_no, rec_id.slot_no);
    }
    Bitmap::reset(page_container.bitmap, rec_id.slot_no);
    if (page_container.page_hdr->num_records-- == file_hdr_.num_records_per_page) {
        release_page_handle(page_container);
    }
    page_container.page->WUnlatch();
    buffer_pool_manager_->unpin_page(page_container.page->get_page_id(), true);
}

/**
 * @description: 更新指定记录号`rec_id`的记录
 * @param {Rid&} rec_id 要更新的记录号
 * @param {char*} new_record_data_ptr 新记录数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid &rec_id, char *new_record_data_ptr, Context *context) {
    // 步骤：
    // 1. 获取指定记录所在的页面句柄
    // 2. 检查记录是否存在
    // 3. 复制新数据到记录槽位
    // 4. unpin页面
    // 如果更新同一记录，由间隙锁保护，无需页锁
    auto &&page_container = fetch_page_handle(rec_id.page_no);
    if (!Bitmap::is_set(page_container.bitmap, rec_id.slot_no)) {
        throw RecordNotFoundError(rec_id.page_no, rec_id.slot_no);
    }
    memcpy(page_container.get_slot(rec_id.slot_no), new_record_data_ptr, file_hdr_.record_size);
    buffer_pool_manager_->unpin_page(page_container.page->get_page_id(), true);
}

/**
 * 以下函数为辅助函数，仅提供参考
*/
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_num 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_num) const {
    if (page_num >= file_hdr_.num_pages || page_num < 0) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), page_num);
    }
    auto &&page = buffer_pool_manager_->fetch_page({fd_, page_num});
    if (page == nullptr) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), page_num);
    }
    return {&file_hdr_, page};
}

/**
 * @description: 创建一个新的页面句柄
 * @return {RmPageHandle} 新的页面句柄
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    PageId new_page_id{fd_, INVALID_PAGE_ID};
    auto &&new_page = buffer_pool_manager_->new_page(&new_page_id);
    if (new_page == nullptr) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), new_page_id.page_no);
    }
    RmPageHandle new_page_handle{&file_hdr_, new_page};
    new_page_handle.page_hdr->num_records = 0;
    new_page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    Bitmap::init(new_page_handle.bitmap, file_hdr_.bitmap_size);
    ++file_hdr_.num_pages;
    file_hdr_.first_free_page_no = new_page->get_page_id().page_no;
    return new_page_handle;
}

/**
 * @brief 创建或获取一个空闲的页面句柄
 * @return RmPageHandle 返回生成的空闲页面句柄
 * @note 调用者必须记得unpin页面！
 */
RmPageHandle RmFileHandle::create_page_handle() {
    std::lock_guard lock(latch_);
    if (file_hdr_.first_free_page_no == INVALID_PAGE_ID) {
        return create_new_page_handle();
    }
    return fetch_page_handle(file_hdr_.first_free_page_no);
}

/**
 * @description: 当页面从满变空闲时，更新文件头和页头中的空闲页面元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle &page_container) {
    page_container.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_container.page->get_page_id().page_no;
}