#include "buffer_pool_manager.h"
#include "page_handle.h"

/**
 * @brief BasicPageHandle的移动构造函数。
 * @param that 待移动的BasicPageGuard对象。
 */
BasicPageHandle::BasicPageHandle(BasicPageHandle &&that) noexcept {
    // 移动that的数据成员到当前对象。
    this->bpm_ = that.bpm_;
    this->page_ = that.page_;
    this->is_dirty_ = that.is_dirty_;

    // 清空that的状态，防止重复释放。
    that.bpm_ = nullptr;
    that.page_ = nullptr;
    that.is_dirty_ = false;
}

/**
 * @brief BasicPageHandle的移动赋值运算符。
 * @param that 待移动的BasicPageGuard对象。
 * @return 对当前对象的引用。
 */
auto BasicPageHandle::operator=(BasicPageHandle &&that) noexcept -> BasicPageHandle & {
    // 首先释放当前对象持有的资源。
    this->Drop();

    // 从that中接管资源。
    this->bpm_ = that.bpm_;
    this->page_ = that.page_;
    this->is_dirty_ = that.is_dirty_;

    // 清空that的状态。
    that.bpm_ = nullptr;
    that.page_ = nullptr;
    that.is_dirty_ = false;

    return *this;
}

/**
 * @brief 释放当前页守卫持有的页面资源。
 */
void BasicPageHandle::Drop() {
    // 只有在bpm和page指针有效时才执行unpin操作。
    bool bpm_valid = (bpm_ != nullptr);
    bool page_valid = (page_ != nullptr);
    bool can_unpin = bpm_valid && page_valid;
    if (can_unpin) {
        PageId current_page_id = page_->get_page_id();
        bool dirty_flag = this->is_dirty_;
        
        // 调用缓冲池管理器unpin页面。
        bpm_->unpin_page(current_page_id, dirty_flag);
    }
    
    // 清空所有成员变量。
    this->bpm_ = nullptr;
    this->page_ = nullptr;
    this->is_dirty_ = false;
}

/**
 * @brief BasicPageHandle的析构函数，确保资源被释放。
 */
BasicPageHandle::~BasicPageHandle() {
    this->Drop();
}

/**
 * @brief 将当前页守卫升级为读页守卫。
 * @return 一个新的ReadPageGuard对象。
 */
auto BasicPageHandle::UpgradeRead() -> ReadPageGuard {
    // 升级为只读：先加读锁
    Page *target = page_;
    target->RLatch();
    
    BufferPoolManager *manager_ptr = this->bpm_;
    Page *guarded_page_ptr = this->page_;
    
    ReadPageGuard tmp_guard(manager_ptr, guarded_page_ptr);
    
    // 释放当前守卫持有的资源，但保留latch。
    this->bpm_ = nullptr;
    this->page_ = nullptr;
    this->is_dirty_ = false;
    
    return tmp_guard;
}

/**
 * @brief 将当前页守卫升级为写页守卫。
 * @return 一个新的WritePageGuard对象。
 */
auto BasicPageHandle::UpgradeWrite() -> WritePageGuard {
    // 升级为写：先加写锁
    Page *write_target = page_;
    write_target->WLatch();
    
    BufferPoolManager *manager_ref = bpm_;
    Page *page_ref = page_;
    
    WritePageGuard tmp_write_guard(manager_ref, page_ref);
    
    // 释放当前守卫持有的资源，但保留latch。
    this->bpm_ = nullptr;
    this->page_ = nullptr;
    this->is_dirty_ = false;
    
    return tmp_write_guard;
}

/**
 * @brief ReadPageGuard的移动构造函数。
 * @param that 待移动的ReadPageGuard对象。
 */
ReadPageGuard::ReadPageGuard(ReadPageGuard &&that) noexcept = default;

/**
 * @brief ReadPageGuard的移动赋值运算符。
 * @param that 待移动的ReadPageGuard对象。
 * @return 对当前对象的引用。
 */
auto ReadPageGuard::operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard & {
    // 释放当前守卫的读锁。
    Page *current_page = guard_.page_;
    bool has_page = (current_page != nullptr);
    if (has_page) {
        current_page->RUnlatch();
    }
    
    // 移动that的资源。
    guard_ = std::move(that.guard_);
    
    return *this;
}

/**
 * @brief 释放当前读页守卫的资源。
 */
void ReadPageGuard::Drop() {
    Page *page_to_unlatch = guard_.page_;
    bool has_unlatch_target = (page_to_unlatch != nullptr);
    if (has_unlatch_target) {
        page_to_unlatch->RUnlatch();
    }
    
    guard_.Drop();
}

/**
 * @brief ReadPageGuard的析构函数。
 */
ReadPageGuard::~ReadPageGuard() {
    this->Drop();
}

/**
 * @brief WritePageGuard的移动构造函数。
 * @param that 待移动的WritePageGuard对象。
 */
WritePageGuard::WritePageGuard(WritePageGuard &&that) noexcept = default;

/**
 * @brief WritePageGuard的移动赋值运算符。
 * @param that 待移动的WritePageGuard对象。
 * @return 对当前对象的引用。
 */
auto WritePageGuard::operator=(WritePageGuard &&that) noexcept -> WritePageGuard & {
    // 释放当前守卫的写锁。
    Page *target_page = guard_.page_;
    bool need_unlatch = (target_page != nullptr);
    if (need_unlatch) {
        target_page->WUnlatch();
    }

    // 移动that的资源。
    guard_ = std::move(that.guard_);
    
    return *this;
}

/**
 * @brief 释放当前写页守卫的资源。
 */
void WritePageGuard::Drop() {
    Page *page_to_unlatch = guard_.page_;
    bool write_unlatch = (page_to_unlatch != nullptr);
    if (write_unlatch) {
        page_to_unlatch->WUnlatch();
    }
    
    guard_.Drop();
}

/**
 * @brief WritePageGuard的析构函数。
 */
WritePageGuard::~WritePageGuard() {
    this->Drop();
}