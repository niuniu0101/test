#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string &db_name) {
    struct stat st;
    int stat_ret = stat(db_name.c_str(), &st);
    bool stat_ok = stat_ret == 0;
    bool is_directory = S_ISDIR(st.st_mode);
    bool result = stat_ok && is_directory;
    return result;
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string &db_name) {
    bool dir_exists = is_dir(db_name);
    if (dir_exists) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string mk_cmd = "mkdir " + db_name;
    int mk_ret = system(mk_cmd.c_str());
    bool mk_failed = mk_ret < 0;
    if (mk_failed) {
        // 创建一个名为db_name的目录
        throw UnixError();
    }
    int cd_ret = chdir(db_name.c_str());
    bool cd_failed = cd_ret < 0;
    if (cd_failed) {
        // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db; // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    int back_ret = chdir("..");
    bool back_failed = back_ret < 0;
    if (back_failed) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string &db_name) {
    bool not_exist = !is_dir(db_name);
    if (not_exist) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string rm_cmd = "rm -r " + db_name;
    int rm_ret = system(rm_cmd.c_str());
    bool rm_failed = rm_ret < 0;
    if (rm_failed) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string &db_name) {
    // 数据库不存在
    bool not_exist = !is_dir(db_name);
    if (not_exist) {
        throw DatabaseNotFoundError(db_name);
    }
    // 数据库已经打开
    bool already_open = !db_.name_.empty();
    if (already_open) {
        throw DatabaseExistsError(db_name);
    }
    // 将当前工作目录设置为数据库目录
    int cd_ret = chdir(db_name.c_str());
    bool cd_failed = cd_ret < 0;
    if (cd_failed) {
        throw UnixError();
    }

    // 从 DB_META_NAME 文件中加载数据库元数据
    std::ifstream ifs(DB_META_NAME);
    bool open_failed = ifs.fail();
    if (open_failed) {
        throw UnixError();
    }
    ifs >> db_; // 注意：此处重载了操作符>>

    // 打开数据库中每个表的记录文件并读入
    for (auto &entry: db_.tabs_) {
        auto &table_name = entry.first;
        auto &tab_meta = entry.second;
        auto file_handle = rm_manager_->open_file(table_name);
        fhs_[table_name] = std::move(file_handle);
        // 待索引完成后更新
        for (auto &index_entry: tab_meta.indexes) {
            auto &index_name = index_entry.first;
            auto &index_meta = index_entry.second;
            std::ignore = index_meta;
            auto index_handle = ix_manager_->open_index(index_name);
            ihs_[index_name] = std::move(index_handle);
        }
    }
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME, std::ios::trunc);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {

    // 记录文件落盘
    std::cout << "before file: " << std::endl;

    // 索引文件落盘
    std::cout << "before index file: " << std::endl;

}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context
 */
void SmManager::show_tables(Context *context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry: db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

/**
 * @description: 显示该表所有的索引,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {String} table_name {Context*} context
 */
void SmManager::show_indexs(std::string &table_name, Context *context) {
    bool is_tab = db_.is_table(table_name);
    if (!is_tab) {
        throw TableNotFoundError(table_name);
    }

    std::ofstream outfile("output.txt", std::ios::out | std::ios::app);
    RecordPrinter printer(3);
    std::vector<std::string> rec_str{table_name, "unique", ""};
    std::string format = "| " + table_name + " | unique | (";

    std::ostringstream cols_stream;
    for (const auto &index_entry: db_.tabs_[table_name].indexes) {
        auto &index_name = index_entry.first;
        auto &index = index_entry.second;
        std::ignore = index_name;
        cols_stream << "(" << index.cols[0].second.name;
        outfile << format << index.cols[0].second.name;
        for (size_t i = 1; i < index.cols.size(); ++i) {
            cols_stream << "," << index.cols[i].second.name;
            outfile << "," << index.cols[i].second.name;
        }
        outfile << ") |\n";
        cols_stream << ")";
        rec_str[2] = cols_stream.str();
        printer.print_indexs(rec_str, context);
        cols_stream.str("");
    }
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context
 */
void SmManager::desc_table(const std::string &tab_name, Context *context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col: tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context
 */
void SmManager::create_table(const std::string &tab_name, const std::vector<ColDef> &col_defs, Context *context) {
    bool exists = db_.is_table(tab_name);
    if (exists) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def: col_defs) {
        ColMeta col = {
            .tab_name = tab_name,
            .name = col_def.name,
            .type = col_def.type,
            .len = col_def.len,
            .offset = curr_offset,
            .index = false
        };
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset; // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    auto file_handle = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, std::move(file_handle));

    flush_meta();

}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string &tab_name, Context *context) {
    bool exists = db_.is_table(tab_name);
    if (!exists) {
        throw TableNotFoundError(tab_name);
    }

    // 表级 X 锁
    // if (context != nullptr) {
    //     context->lock_mgr_->lock_exclusive_on_table(context->txn_, fhs_[tab_name]->GetFd());
    // }

    auto &tab_meta = db_.get_table(tab_name);

    // 先关闭再删除表文件
    rm_manager_->close_file(fhs_[tab_name].get());
    rm_manager_->destroy_file(tab_name);

    // 先关闭再删除索引文件
    for (auto &index_entry: tab_meta.indexes) {
        auto &index_name = index_entry.first;
        auto &index_meta = index_entry.second;
        std::ignore = index_meta;
        ix_manager_->close_index(ihs_[index_name].get());
        ix_manager_->destroy_index(index_name);
        ihs_.erase(index_name);
    }

    fhs_.erase(tab_name);
    db_.tabs_.erase(tab_name);

    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(std::string &tab_name, std::vector<std::string> &col_names, Context *context) {
    auto &&ix_name = ix_manager_->get_index_name(tab_name, col_names);
    bool index_file_exists = disk_manager_->is_file(ix_name);
    if (index_file_exists) {
        throw IndexExistsError(tab_name, col_names);
    }

    // 异常情况检查放前面
    auto &table_meta = db_.get_table(tab_name);

    // 表级 S 锁
    // 建立索引要读表上的所有记录，所以申请表级读锁
    // if (context != nullptr && context->lock_mgr_ != nullptr) {
    //     context->lock_mgr_->lock_shared_on_table(context->txn_, fhs_[tab_name]->GetFd());
    // }

    std::vector<ColMeta> col_metas;
    col_metas.reserve(col_names.size());
    auto total_len = 0;
    for (auto &col_name: col_names) {
        auto col_meta = *table_meta.get_col(col_name);
        col_metas.emplace_back(col_meta);
        total_len += col_metas.back().len;
    }

    // auto ix_name = std::move(ix_manager_->get_index_name(tab_name, col_names));
    ix_manager_->create_index(ix_name, col_metas);
    auto &&ih = ix_manager_->open_index(ix_name);
    auto &&fh = fhs_[tab_name];

    int offset = 0;
    char *key = new char[total_len];
    for (auto &&scan = std::make_unique<RmScan>(fh.get()); !scan->is_end(); scan->next()) {
        auto &&rid = scan->rid();
        auto &&record = fh->get_record(rid, context);
        offset = 0;
        for (auto &col_meta: col_metas) {
            memcpy(key + offset, record->data + col_meta.offset, col_meta.len);
            offset += col_meta.len;
        }
        // 插入B+树
        int insert_result = ih->insert_entry(key, rid, context->txn_);
        bool dup = insert_result == IX_NO_PAGE;
        if (dup) {
            // 释放内存
            delete []key;
            // 重复了
            ix_manager_->close_index(ih.get());
            ix_manager_->destroy_index(ix_name);
            // drop_index(tab_name, col_names, context);
            throw NonUniqueIndexError(tab_name, col_names);
        }
    }
    // 释放内存
    delete []key;

    // 更新表元索引数据
    IndexMeta new_index(std::move(tab_name), total_len, static_cast<int>(col_names.size()), std::move(col_metas));
    table_meta.indexes.emplace(ix_name, std::move(new_index));
    // 插入索引句柄
    ihs_[std::move(ix_name)] = std::move(ih);
    // 持久化
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string &tab_name, const std::vector<std::string> &col_names, Context *context) {
    auto &table_meta = db_.get_table(tab_name);
    auto &&ix_name = ix_manager_->get_index_name(tab_name, col_names);
    bool present = disk_manager_->is_file(ix_name);
    if (!present) {
        throw IndexNotFoundError(tab_name, col_names);
    }

    // 表级 S 锁
    // 删除索引时只允许对表读操作，写操作可能会误写将被删除的索引，所以申请表级读锁
    // if (context != nullptr) {
    //     context->lock_mgr_->lock_shared_on_table(context->txn_, fhs_[tab_name]->GetFd());
    // }

    ix_manager_->close_index(ihs_[ix_name].get());
    ix_manager_->destroy_index(ix_name);
    ihs_.erase(ix_name);
    table_meta.indexes.erase(ix_name);
    // 持久化
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string &tab_name, const std::vector<ColMeta> &cols, Context *context) {
    auto &table_meta = db_.get_table(tab_name);
    auto &&ix_name = ix_manager_->get_index_name(tab_name, cols);
    bool exists = disk_manager_->is_file(ix_name);
    if (!exists) {
        std::vector<std::string> col_names;
        col_names.reserve(cols.size());
        for (auto &col: cols) {
            col_names.emplace_back(col.name);
        }
        throw IndexNotFoundError(tab_name, col_names);
    }

    // 表级 S 锁
    // 删除索引时只允许对表读操作，写操作可能会误写将被删除的索引，所以申请表级读锁
    // if (context != nullptr) {
    //     context->lock_mgr_->lock_shared_on_table(context->txn_, fhs_[tab_name]->GetFd());
    // }

    ix_manager_->close_index(ihs_[ix_name].get());
    ix_manager_->destroy_index(ix_name);
    ihs_.erase(ix_name);
    table_meta.indexes.erase(ix_name);
    // 持久化
    flush_meta();
}

/**
 * @description: 仅用于恢复索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::redo_index(const std::string &tab_name, TabMeta &table_meta, const std::vector<std::string> &col_names,
                           const std::string &index_name, Context *context) {
    ix_manager_->close_index(ihs_[index_name].get());
    ix_manager_->destroy_index(index_name);

    std::vector<ColMeta> col_metas;
    col_metas.reserve(col_names.size());
    auto total_len = 0;
    for (auto &col_name: col_names) {
        auto col_meta = *table_meta.get_col(col_name);
        col_metas.emplace_back(col_meta);
        total_len += col_metas.back().len;
    }

    // auto ix_name = std::move(ix_manager_->get_index_name(tab_name, col_names));
    ix_manager_->create_index(index_name, col_metas);
    auto &&ih = ix_manager_->open_index(index_name);
    auto &&fh = fhs_[tab_name];

    int offset = 0;
    char *key = new char[total_len];
    for (auto &&scan = std::make_unique<RmScan>(fh.get()); !scan->is_end(); scan->next()) {
        auto &&rid = scan->rid();
        auto &&record = fh->get_record(rid, context);
        offset = 0;
        for (auto &col_meta: col_metas) {
            memcpy(key + offset, record->data + col_meta.offset, col_meta.len);
            offset += col_meta.len;
        }
        // 插入B+树
        int insert_result = ih->insert_entry(key, rid, context->txn_);
        bool dup = insert_result == IX_NO_PAGE;
        if (dup) {
            // 释放内存
            delete []key;
            // 重复了
            ix_manager_->close_index(ih.get());
            ix_manager_->destroy_index(index_name);
            // drop_index(tab_name, col_names, context);
            throw NonUniqueIndexError(tab_name, col_names);
        }
    }
    // 释放内存
    delete []key;

    // 插入索引句柄
    ihs_[index_name] = std::move(ih);
}
