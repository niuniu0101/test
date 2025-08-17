#pragma once

#include "rm_defs.h"
#include "rm_file_handle.h"

class RmFileHandle;

class RmScan : public RecScan {
private:
    const RmFileHandle *file_handle_;
    RmPageHandle cur_page_handle_;
    Rid rid_;

public:
    RmScan(const RmFileHandle *file_handle);

    void next() override;

    bool is_end() const override;

    Rid rid() const override;

    std::unique_ptr<RmRecord> get_record();
};
