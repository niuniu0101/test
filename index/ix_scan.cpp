#include "ix_scan.h"

/**
 * @brief 将扫描器前进到索引中的下一个条目。
 * @note 此方法处理在叶节点内移动到下一个槽，并在到达当前叶节点的末尾时
 * 负责过渡到下一个叶节点。
 */
void IxScan::next() {
    // 增加槽号以移动到下一个条目。
    int next_slot = iid_.slot_no + 1;
    iid_.slot_no = next_slot;
    int current_size = cur_node_handle_->get_size();

    // 检查是否已到达当前叶节点的末尾。
    bool at_end_of_current_leaf = (iid_.slot_no == current_size);
    bool is_not_last_leaf = (iid_.page_no != ih_->file_hdr_->last_leaf_);

    if (is_not_last_leaf && at_end_of_current_leaf) {
        // 如果是，我们需要移动到下一个叶页面。
        int next_page_id = cur_node_handle_->get_next_leaf();

        // 释放当前页面的读锁并unpin，以释放资源。
        cur_node_handle_->page->RUnlatch();
        bpm_->unpin_page(cur_node_handle_->page->get_page_id(), false);

        // 获取新的叶节点并获取读锁。
        iid_.slot_no = 0;
        iid_.page_no = next_page_id;
        cur_node_handle_ = ih_->fetch_node(iid_.page_no);
        cur_node_handle_->page->RLatch();
    }
}

Rid IxScan::rid() const {
    return ih_->get_rid(iid_);
}

Iid IxScan::prev_iid(const Iid &iid) {
    int current_slot = iid.slot_no;
    int target_slot = current_slot - 1;

    // 检查是否可以在同一页面内移动到上一个槽。
    if (target_slot >= 0) {
        Iid prev_entry_iid = {iid.page_no, target_slot};
        return prev_entry_iid;
    }

    // 如果我们在第一个槽，必须转到上一个叶节点。
    auto current_node = ih_->fetch_node(iid.page_no);
    current_node->page->RLatch();

    // 确保我们位于叶页面上。
    assert(current_node->is_leaf_page());

    bool is_first_leaf = (current_node->get_page_no() == ih_->file_hdr_->first_leaf_);

    if (!is_first_leaf) {
        // 获取上一个叶节点。
        page_id_t prev_page_id = current_node->get_prev_leaf();
        auto prev_node = ih_->fetch_node(prev_page_id);
        prev_node->page->RLatch();

        // 上一个条目位于上一个叶节点的最后一个槽位。
        int last_slot_of_prev_node = prev_node->get_size() - 1;
        Iid result_iid = {prev_node->get_page_no(), last_slot_of_prev_node};

        // 释放锁并unpin页面以进行清理。
        current_node->page->RUnlatch();
        prev_node->page->RUnlatch();
        bpm_->unpin_page(prev_node->get_page_id(), false);
        bpm_->unpin_page(current_node->get_page_id(), false);

        return result_iid;
    }

    // 在正常情况下不应该到达此处，因为第一个叶节点的第一个条目没有上一个条目。
    assert(0);
    current_node->page->RUnlatch();
    bpm_->unpin_page(current_node->get_page_id(), false);
    return {-1, -1};
}

Rid IxScan::prev_rid(const Iid &iid) {
    return ih_->get_rid(prev_iid(iid));
}

RmRecord IxScan::get_key() {
    return ih_->get_key(iid_);
}