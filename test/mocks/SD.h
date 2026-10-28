// ============================================================================
// PC上でのテスト用 SD.h モック (メモリ上のファイルシステム)
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#define FILE_READ 0
#define FILE_WRITE 1

// テスト用: 書き込めるバイト数の残り (これを超える書き込みは失敗する)
inline size_t mock_sd_write_budget = SIZE_MAX;

class File {
   public:
    File() {}
    File(std::shared_ptr<std::vector<uint8_t>> d, bool append)
        : data(d), valid(true), pos(append ? d->size() : 0) {}

    operator bool() const { return valid; }

    int read(uint8_t* buf, size_t len) {
        if (!valid) {
            return 0;
        }
        size_t n = (pos < data->size()) ? data->size() - pos : 0;
        if (n > len) {
            n = len;
        }
        memcpy(buf, data->data() + pos, n);
        pos += n;
        return (int)n;
    }
    int read() {
        uint8_t c;
        return (read(&c, 1) == 1) ? c : -1;
    }

    size_t write(const uint8_t* buf, size_t len) {
        if (!valid || len > mock_sd_write_budget) {
            return 0;
        }
        mock_sd_write_budget -= len;
        if (data->size() < pos + len) {
            data->resize(pos + len);
        }
        memcpy(data->data() + pos, buf, len);
        pos += len;
        return len;
    }
    size_t write(uint8_t c) { return write(&c, 1); }

    bool seek(uint32_t p) {
        if (!valid || p > data->size()) {
            return false;
        }
        pos = p;
        return true;
    }
    uint32_t position() const { return (uint32_t)pos; }
    uint32_t size() const { return valid ? (uint32_t)data->size() : 0; }
    int available() const {
        return (valid && pos < data->size()) ? (int)(data->size() - pos) : 0;
    }
    void flush() {}
    void close() { valid = false; }

   private:
    std::shared_ptr<std::vector<uint8_t>> data;
    bool valid = false;
    size_t pos = 0;
};

class SDClass {
   public:
    File open(const char* path, int mode = FILE_READ) {
        auto it = files.find(path);
        if (mode == FILE_READ) {
            if (it == files.end()) {
                return File();
            }
            return File(it->second, false);
        }
        if (it == files.end()) {
            it = files.emplace(path, std::make_shared<std::vector<uint8_t>>()).first;
        }
        return File(it->second, true);
    }
    bool exists(const char* path) { return files.count(path) != 0; }
    bool remove(const char* path) { return files.erase(path) != 0; }
    bool rename(const char* from, const char* to) {
        auto it = files.find(from);
        if (it == files.end()) {
            return false;
        }
        files[to] = it->second;
        files.erase(from);
        return true;
    }

    // --- テスト用 ---
    void put(const char* path, const std::vector<uint8_t>& d) {
        files[path] = std::make_shared<std::vector<uint8_t>>(d);
    }
    std::vector<uint8_t> get(const char* path) {
        auto it = files.find(path);
        return (it == files.end()) ? std::vector<uint8_t>() : *it->second;
    }
    void clear() { files.clear(); }

   private:
    std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files;
};

inline SDClass SD;
