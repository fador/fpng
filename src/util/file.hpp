#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <span>
#include <filesystem>

namespace fpng {

class FileReader {
public:
    FileReader() = default;
    ~FileReader();

    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    FileReader(FileReader&& other) noexcept;
    FileReader& operator=(FileReader&& other) noexcept;

    bool open(const std::filesystem::path& path);
    void close();

    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool is_open() const noexcept { return data_ != nullptr; }
    explicit operator bool() const noexcept { return is_open(); }

    std::span<const uint8_t> span() const noexcept {
        return {data_, size_};
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;

#ifdef _WIN32
    void* file_handle_ = nullptr;
    void* mapping_handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

std::vector<uint8_t> read_file(const std::filesystem::path& path);

bool write_file(const std::filesystem::path& path,
                const uint8_t* data, size_t size);

inline bool write_file(const std::filesystem::path& path,
                       std::span<const uint8_t> data) {
    return write_file(path, data.data(), data.size());
}

} // namespace fpng
