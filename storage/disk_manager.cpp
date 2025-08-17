#include "storage/disk_manager.h"

#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "defs.h"

DiskManager::DiskManager() {
    memset(fd2pageno_, 0, MAX_FD * sizeof(std::atomic<page_id_t>));
}

/**
 * @description: 将数据写入到磁盘文件的指定页面。
 * @param {int} fd 目标磁盘文件的文件句柄。
 * @param {page_id_t} page_no 目标页面的编号。
 * @param {const char*} data 要写入的数据。
 * @param {int} num_bytes 要写入的数据量大小。
 */
void DiskManager::write_page(int fd, page_id_t page_no, const char *data, int num_bytes) {
    // 计算页面在文件中的偏移量。
    off_t target_offset = static_cast<off_t>(page_no) * PAGE_SIZE;

    // 定位文件指针到目标偏移量。
    if (lseek(fd, target_offset, SEEK_SET) == -1) {
        perror("Error seeking to write position");
        throw InternalError("DiskManager::write_page: lseek failed.");
    }
    
    // 写入数据并验证写入字节数。
    ssize_t bytes_written = write(fd, data, num_bytes);
    if (bytes_written != num_bytes) {
        throw InternalError("DiskManager::write_page: Write operation failed to write all bytes.");
    }
}

/**
 * @description: 从文件中指定的页面读取数据到内存中。
 * @param {int} fd 目标磁盘文件的文件句柄。
 * @param {page_id_t} page_no 目标页面的编号。
 * @param {char*} data 用于存放读取内容的内存地址。
 * @param {int} num_bytes 要读取的数据量大小。
 */
void DiskManager::read_page(int fd, page_id_t page_no, char *data, int num_bytes) {
    // 计算页面在文件中的偏移量。
    off_t page_offset = static_cast<off_t>(page_no) * PAGE_SIZE;

    // 定位文件指针到目标偏移量。
    if (lseek(fd, page_offset, SEEK_SET) == -1) {
        perror("Error seeking to read position");
        throw InternalError("DiskManager::read_page: lseek failed.");
    }

    // 读取数据并验证读取字节数。
    ssize_t bytes_read = read(fd, data, num_bytes);
    if (bytes_read != num_bytes) {
        throw InternalError("DiskManager::read_page: Read operation failed to read all bytes.");
    }
}

/**
 * @description: 为指定文件分配一个新的页面编号。
 * @return {page_id_t} 分配的新页面编号。
 * @param {int} fd 目标文件的文件句柄。
 */
page_id_t DiskManager::allocate_page(int fd) {
    // 使用原子操作，确保线程安全地自增页号。
    assert(fd >= 0 && fd < MAX_FD);
    page_id_t next_page_id = fd2pageno_[fd]++;
    return next_page_id;
}

void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {
    // 此函数在当前实现中为空，用于满足接口要求。
}

/**
 * @description: 检查指定路径是否为目录。
 * @return {bool} 如果路径是目录，则返回true；否则返回false。
 * @param {const std::string&} path 目标路径。
 */
bool DiskManager::is_dir(const std::string &path) {
    struct stat file_stats;
    return (stat(path.c_str(), &file_stats) == 0 && S_ISDIR(file_stats.st_mode));
}

/**
 * @description: 创建一个新目录。
 * @param {const std::string&} path 要创建的目录路径。
 */
void DiskManager::create_dir(const std::string &path) {
    std::string command = "mkdir " + path;
    if (system(command.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除一个目录及其内容。
 * @param {const std::string&} path 要删除的目录路径。
 */
void DiskManager::destroy_dir(const std::string &path) {
    std::string command = "rm -r " + path;
    if (system(command.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 检查指定路径是否为普通文件。
 * @return {bool} 如果路径是文件，则返回true；否则返回false。
 * @param {const std::string&} path 目标路径。
 */
bool DiskManager::is_file(const std::string &path) {
    struct stat file_status;
    return (stat(path.c_str(), &file_status) == 0 && S_ISREG(file_status.st_mode));
}

/**
 * @description: 创建一个新文件。
 * @param {const std::string&} path 要创建的文件路径。
 */
void DiskManager::create_file(const std::string &path) {
    if (is_file(path)) {
        throw FileExistsError(path);
    }
    
    // 使用O_CREAT | O_EXCL确保文件不存在时才创建。
    int file_descriptor = open(path.c_str(), O_CREAT | O_EXCL, 0644);
    if (file_descriptor == -1) {
        throw InternalError("DiskManager::create_file: Failed to open/create file.");
    }
    
    // 立即关闭文件。
    if (close(file_descriptor) == -1) {
        throw InternalError("DiskManager::create_file: Failed to close the new file.");
    }
}

/**
 * @description: 删除指定路径的文件。
 * @param {const std::string&} path 要删除的文件的路径。
 */
void DiskManager::destroy_file(const std::string &path) {
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    
    // 检查文件是否已打开。
    if (path2fd_.count(path)) {
        throw FileNotClosedError(path);
    }
    
    // 调用unlink删除文件。
    if (unlink(path.c_str()) == -1) {
        throw InternalError("DiskManager::destroy_file: Failed to unlink file.");
    }
}

/**
 * @description: 打开指定路径的文件，并返回其文件句柄。
 * @return {int} 打开文件的文件句柄。
 * @param {const std::string&} path 要打开的文件路径。
 */
int DiskManager::open_file(const std::string &path) {
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    
    // 检查文件是否已打开。
    if (path2fd_.count(path)) {
        throw FileNotClosedError(path);
    }
    
    int file_handle = open(path.c_str(), O_RDWR);
    if (file_handle == -1) {
        throw InternalError("DiskManager::open_file: Failed to open file for read/write.");
    }
    
    // 更新文件映射表。
    path2fd_[path] = file_handle;
    fd2path_[file_handle] = path;
    
    return file_handle;
}

/**
 * @description: 关闭指定文件句柄所对应的文件。
 * @param {int} fd 要关闭的文件的文件句柄。
 */
void DiskManager::close_file(int fd) {
    // 检查文件句柄是否有效。
    if (fd2path_.count(fd) == 0) {
        throw FileNotOpenError(fd);
    }
    
    // 从映射表中移除记录。
    path2fd_.erase(fd2path_[fd]);
    fd2path_.erase(fd);
    
    // 调用close函数。
    if (close(fd) == -1) {
        throw InternalError("DiskManager::close_file: Failed to close file.");
    }
}

/**
 * @description: 获取文件的大小（以字节为单位）。
 * @return {int} 文件的大小；若失败则返回-1。
 * @param {const std::string&} file_name 目标文件名。
 */
int DiskManager::get_file_size(const std::string &file_name) {
    struct stat file_stats;
    int ret_code = stat(file_name.c_str(), &file_stats);
    return ret_code == 0 ? static_cast<int>(file_stats.st_size) : -1;
}

/**
 * @description: 根据文件句柄获取文件名。
 * @return {std::string} 文件名。
 * @param {int} fd 文件句柄。
 */
std::string DiskManager::get_file_name(int fd) {
    if (!fd2path_.count(fd)) {
        throw FileNotOpenError(fd);
    }
    return fd2path_[fd];
}

/**
 * @description: 根据文件名获取文件句柄。如果文件未打开，则会尝试打开。
 * @return {int} 文件的文件句柄。
 * @param {const std::string&} file_name 目标文件名。
 */
int DiskManager::get_file_fd(const std::string &file_name) {
    auto file_handle_iter = path2fd_.find(file_name);
    if (file_handle_iter != path2fd_.end()) {
        return file_handle_iter->second;
    }
    return open_file(file_name);
}

/**
 * @description: 从日志文件中读取数据。
 * @return {int} 读取的字节数；如果读取位置超过文件大小，则返回-1。
 * @param {char*} log_data 存放读取内容的缓冲区。
 * @param {int} size 要读取的字节数。
 * @param {int} offset 文件中的起始偏移量。
 */
int DiskManager::read_log(char *log_data, int size, int offset) {
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    
    int file_size = get_file_size(LOG_FILE_NAME);
    if (offset > file_size) {
        return -1;
    }
    
    int actual_read_size = std::min(size, file_size - offset);
    if (actual_read_size == 0) return 0;
    
    lseek(log_fd_, offset, SEEK_SET);
    ssize_t bytes_read = read(log_fd_, log_data, actual_read_size);
    assert(bytes_read == actual_read_size);
    return bytes_read;
}

/**
 * @description: 将数据写入日志文件。
 * @param {char*} log_data 要写入的日志数据。
 * @param {int} size 要写入的字节数。
 */
void DiskManager::write_log(char *log_data, int size) {
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    
    // 将文件指针移动到文件末尾。
    if (lseek(log_fd_, 0, SEEK_END) == -1) {
        throw UnixError();
    }
    
    ssize_t bytes_written = write(log_fd_, log_data, size);
    if (bytes_written != size) {
        throw UnixError();
    }
}