#include "cmt_sd_buffer.h"

// ============================================================================
// SdBufferedRead 実装
// ============================================================================
SdBufferedRead::SdBufferedRead(File* f, size_t buffer_size)
    : file(f), buf_size(buffer_size), pos(0), available_bytes(0), file_pos(0) {
    buf = new uint8_t[buf_size];
}

SdBufferedRead::~SdBufferedRead() { delete[] buf; }

bool SdBufferedRead::read_word(uint16_t& out_word) {
    if (pos + 2 > available_bytes) {
        size_t remaining = available_bytes - pos;
        if (remaining > 0) {
            memmove(buf, buf + pos, remaining);
        }
        file_pos = file->position();
        size_t read_bytes = file->read(buf + remaining, buf_size - remaining);
        available_bytes = remaining + read_bytes;
        pos = 0;

        if (available_bytes < 2) {
            return false;
        }
    }
    out_word = (uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8);
    pos += 2;
    return true;
}

bool SdBufferedRead::read_byte(uint8_t& out_byte) {
    if (pos + 1 > available_bytes) {
        size_t remaining = available_bytes - pos;
        if (remaining > 0) {
            memmove(buf, buf + pos, remaining);
        }
        file_pos = file->position();
        size_t read_bytes = file->read(buf + remaining, buf_size - remaining);
        available_bytes = remaining + read_bytes;
        pos = 0;

        if (available_bytes < 1) {
            return false;
        }
    }
    out_byte = buf[pos++];
    return true;
}

int SdBufferedRead::read(uint8_t* out_buf, size_t len) {
    size_t bytes_read = 0;
    while (len > 0) {
        if (pos >= available_bytes) {
            file_pos = file->position();
            available_bytes = file->read(buf, buf_size);
            pos = 0;
            if (available_bytes == 0) {
                break;
            }
        }
        size_t to_copy = available_bytes - pos;
        if (to_copy > len) {
            to_copy = len;
        }
        memcpy(out_buf + bytes_read, buf + pos, to_copy);
        pos += to_copy;
        bytes_read += to_copy;
        len -= to_copy;
    }
    return bytes_read;
}

size_t SdBufferedRead::available() {
    if (!file) {
        return 0;
    }
    return (available_bytes - pos) + file->available();
}

bool SdBufferedRead::seek(uint32_t position) {
    if (!file) {
        return false;
    }
    bool success = file->seek(position);
    if (success) {
        file_pos = position;
        pos = 0;
        available_bytes = 0;  // シークした場合はバッファを無効化して読み直す
    }
    return success;
}

uint32_t SdBufferedRead::position() {
    if (!file) {
        return 0;
    }
    return file->position() - (available_bytes - pos);
}

uint32_t SdBufferedRead::display_position() {
    if (!file) {
        return 0;
    }
    return file_pos + pos;
}

// ============================================================================
// SdBufferedWrite 実装
// ============================================================================
SdBufferedWrite::SdBufferedWrite(File* f, size_t buffer_size)
    : file(f), buf_size(buffer_size), pos(0) {
    buf = new uint8_t[buf_size];
}

SdBufferedWrite::~SdBufferedWrite() {
    flush();
    delete[] buf;
}

bool SdBufferedWrite::flush() {
    if (pos > 0) {
        if (file->write(buf, pos) != pos) {
            return false;
        }
        pos = 0;
    }
    return true;
}

bool SdBufferedWrite::write_bytes(const uint8_t* data, size_t len) {
    while (len > 0) {
        size_t space = buf_size - pos;
        if (space == 0) {
            if (!flush()) {
                return false;
            }
            space = buf_size;
        }
        size_t chunk = (len < space) ? len : space;
        memcpy(buf + pos, data, chunk);
        pos += chunk;
        data += chunk;
        len -= chunk;
    }
    return true;
}

bool SdBufferedWrite::write_u16(uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF)};
    return write_bytes(b, 2);
}

bool SdBufferedWrite::write_u32(uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                    (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)};
    return write_bytes(b, 4);
}