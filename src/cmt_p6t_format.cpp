#include "cmt_p6t_format.h"

#include <string.h>

// フッタの最小サイズ ("P6" + 5バイト + コマンド長2 + 拡張情報の長さ2、ブロック無し)
static constexpr uint32_t P6T_MIN_FOOTER = 11;

namespace {

// ファイルの決まった位置から読む小さなリーダー (フッタは数KB以下)
class FooterReader {
   public:
    FooterReader(File& f, uint32_t pos, uint32_t end) : file(f), cur(pos), limit(end) {
        ok = file.seek(pos);
    }

    bool good() const { return ok; }

    bool bytes(void* out, uint32_t len) {
        if (!ok || cur + len > limit) {
            ok = false;
            return false;
        }
        if (file.read((uint8_t*)out, len) != (int)len) {
            ok = false;
            return false;
        }
        cur += len;
        return true;
    }

    uint8_t u8() {
        uint8_t v = 0;
        bytes(&v, 1);
        return v;
    }

    uint16_t u16() {
        uint8_t b[2] = {0, 0};
        bytes(b, 2);
        return (uint16_t)(b[0] | (b[1] << 8));
    }

    uint32_t u32() {
        uint8_t b[4] = {0, 0, 0, 0};
        bytes(b, 4);
        return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
               ((uint32_t)b[3] << 24);
    }

    bool skip(uint32_t len) {
        if (!ok || cur + len > limit) {
            ok = false;
            return false;
        }
        cur += len;
        ok = file.seek(cur);
        return ok;
    }

   private:
    File& file;
    uint32_t cur;
    uint32_t limit;
    bool ok;
};

bool read_footer(File& f, P6TInfo& info) {
    uint32_t file_size = f.size();
    if (file_size < 4 + P6T_MIN_FOOTER) {
        return false;
    }
    FooterReader tail(f, file_size - 4, file_size);
    uint32_t data_size = tail.u32();
    // フッタ (最小 P6T_MIN_FOOTER バイト) の前にベタイメージが収まること
    if (!tail.good() || data_size > file_size - 4 - P6T_MIN_FOOTER) {
        return false;
    }

    FooterReader r(f, data_size, file_size - 4);
    char id[2];
    if (!r.bytes(id, 2) || memcmp(id, P6T_FOOTER_ID, 2) != 0) {
        return false;
    }
    r.u8();  // バージョン (PC6001VX も見ていない)
    uint8_t count = r.u8();
    r.u8();  // オートスタート
    r.u8();  // BASIC モード
    r.u8();  // ページ数
    r.skip(r.u16());  // オートスタートのコマンド
    r.skip(r.u16());  // 拡張情報
    if (!r.good()) {
        return false;
    }

    info.blocks.clear();
    for (int i = 0; i < count; i++) {
        P6TBlock b;
        char bid[2];
        if (!r.bytes(bid, 2) || memcmp(bid, P6T_BLOCK_ID, 2) != 0) {
            return false;
        }
        b.id = r.u8();
        r.bytes(b.name, P6T_NAME_LEN);
        b.baud = r.u16();
        b.stime_ms = r.u16();
        b.ptime_ms = r.u16();
        b.offset = r.u32();
        b.size = r.u32();
        if (!r.good() || b.offset > data_size || b.size > data_size - b.offset) {
            return false;
        }
        if (b.baud != 600 && b.baud != 1200) {
            b.baud = P6T_DEFAULT_BAUD;
        }
        b.name[P6T_NAME_LEN - 1] = '\0';
        info.blocks.push_back(b);
    }
    info.has_footer = true;
    info.data_size = data_size;
    return true;
}

}  // namespace

bool p6t_read_info(File& f, P6TInfo& info) {
    info = P6TInfo();
    if (!f) {
        return false;
    }
    if (read_footer(f, info)) {
        f.seek(0);
        return true;
    }

    // 素の P6 として扱う
    info = P6TInfo();
    info.data_size = f.size();
    P6TBlock b;
    b.stime_ms = P6T_PLAIN_STIME_MS;
    b.ptime_ms = P6T_PLAIN_PTIME_MS;
    b.size = info.data_size;
    info.blocks.push_back(b);
    f.seek(0);
    return info.data_size > 0;
}

bool p6t_write_footer(SdBufferedWrite* w, const std::vector<P6TBlock>& blocks,
                      uint32_t data_size) {
    bool ok = w->write_bytes((const uint8_t*)P6T_FOOTER_ID, 2);
    uint8_t head[5] = {P6T_VERSION, (uint8_t)blocks.size(), 0 /* オートスタート無効 */,
                       1 /* BASIC モード */, 1 /* ページ数 */};
    ok = ok && w->write_bytes(head, sizeof(head));
    ok = ok && w->write_u16(0);  // オートスタートのコマンド無し
    ok = ok && w->write_u16(0);  // 拡張情報無し
    for (const P6TBlock& b : blocks) {
        ok = ok && w->write_bytes((const uint8_t*)P6T_BLOCK_ID, 2);
        ok = ok && w->write_bytes(&b.id, 1);
        ok = ok && w->write_bytes((const uint8_t*)b.name, P6T_NAME_LEN);
        ok = ok && w->write_u16(b.baud);
        ok = ok && w->write_u16(b.stime_ms);
        ok = ok && w->write_u16(b.ptime_ms);
        ok = ok && w->write_u32(b.offset);
        ok = ok && w->write_u32(b.size);
    }
    ok = ok && w->write_u32(data_size);
    return ok;
}
