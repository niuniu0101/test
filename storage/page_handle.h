#ifndef PAGE_GUARD_H
#define PAGE_GUARD_H

#include "page.h"

class BufferPoolManager;
class ReadPageGuard;
class WritePageGuard;

class BasicPageHandle {
public:
    BasicPageHandle() = default;
    BasicPageHandle(BufferPoolManager *bpm, Page *page) : bpm_(bpm), page_(page) {}
    BasicPageHandle(const BasicPageHandle &) = delete;
    auto operator=(const BasicPageHandle &) -> BasicPageHandle & = delete;
    BasicPageHandle(BasicPageHandle &&that) noexcept;
    auto operator=(BasicPageHandle &&that) noexcept -> BasicPageHandle &;
    ~BasicPageHandle();

    auto GetPageId() const -> PageId {
        return page_->get_page_id();
    }
    
    auto GetData() const -> const char * {
        return page_->get_data();
    }

    auto GetDataMut() -> char * {
        is_dirty_ = true;
        return page_->get_data();
    }

    template<class T>
    auto As() -> const T * {
        return reinterpret_cast<const T *>(GetData());
    }

    template<class T>
    auto AsMut() -> T * {
        return reinterpret_cast<T *>(GetDataMut());
    }

    void Drop();
    auto UpgradeRead() -> ReadPageGuard;
    auto UpgradeWrite() -> WritePageGuard;

private:
    friend ReadPageGuard;
    friend WritePageGuard;
    BufferPoolManager *bpm_{nullptr};
    Page *page_{nullptr};
    bool is_dirty_{false};
};

class ReadPageGuard {
public:
    ReadPageGuard() = default;
    ReadPageGuard(BufferPoolManager *bpm, Page *page) : guard_(bpm, page) {}
    ReadPageGuard(const ReadPageGuard &) = delete;
    auto operator=(const ReadPageGuard &) -> ReadPageGuard & = delete;
    ReadPageGuard(ReadPageGuard &&that) noexcept;
    auto operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard &;
    ~ReadPageGuard();

    auto GetPageId() const -> PageId {
        return guard_.GetPageId();
    }
    
    auto GetData() const -> const char * {
        return guard_.GetData();
    }

    template<class T>
    auto As() -> const T * {
        return guard_.As<T>();
    }

    void Drop();

private:
    BasicPageHandle guard_;
};

class WritePageGuard {
public:
    WritePageGuard() = default;
    WritePageGuard(BufferPoolManager *bpm, Page *page) : guard_(bpm, page) {}
    WritePageGuard(const WritePageGuard &) = delete;
    auto operator=(const WritePageGuard &) -> WritePageGuard & = delete;
    WritePageGuard(WritePageGuard &&that) noexcept;
    auto operator=(WritePageGuard &&that) noexcept -> WritePageGuard &;
    ~WritePageGuard();

    auto GetPageId() const -> PageId {
        return guard_.GetPageId();
    }
    
    auto GetData() const -> const char * {
        return guard_.GetData();
    }

    template<class T>
    auto As() -> const T * {
        return guard_.As<T>();
    }

    auto GetDataMut() -> char * {
        return guard_.GetDataMut();
    }

    template<class T>
    auto AsMut() -> const T * {
        return guard_.AsMut<T>();
    }

    void Drop();

private:
    BasicPageHandle guard_;
};
#endif //PAGE_GUARD_H