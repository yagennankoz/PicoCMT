#pragma once

#include <Arduino.h>
#include <SD.h>

class SdBufferedRead {
   private:
    File* file;
    uint8_t* buf;
    size_t buf_size;
    size_t pos;
    size_t available_bytes;
    size_t file_pos;

   public:
    SdBufferedRead(File* f, size_t buffer_size = 4096);
    ~SdBufferedRead();

    bool read_word(uint16_t& out_word);

    bool read_byte(uint8_t& out_byte);
    int read(uint8_t* out_buf, size_t len);
    size_t available();
    bool seek(uint32_t position);
    uint32_t position();
    uint32_t display_position();
};

class SdBufferedWrite {
   private:
    File* file;
    uint8_t* buf;
    size_t buf_size;
    size_t pos;

   public:
    SdBufferedWrite(File* f, size_t buffer_size = 4096);
    ~SdBufferedWrite();

    bool flush();
    bool write_bytes(const uint8_t* data, size_t len);
    bool write_u16(uint16_t v);
    bool write_u32(uint32_t v);
};