import struct
import os

class TapeImageParser:
    """T88, CMT, CASファイルのバイナリパースを専門に行うクラス"""
    TAG_NAMES = {
        0x0000: "End of File (終了タグ)",
        0x0001: "Format Version (バージョン情報)",
        0x0100: "Blank (無音・無データ空間)",
        0x0101: "Data (データブロック)",
        0x0102: "Space (スペース)",
        0x0103: "Mark (マーク)"
    }

    def __init__(self):
        self.file_data = b""
        self.tags_info = []
        self.is_cmt_mode = False
        self.is_cas_mode = False
        self.filename = ""
        self.filesize = 0

    def load_file(self, file_path):
        """ファイルを読み込み、初期判定を行う"""
        with open(file_path, "rb") as f:
            self.file_data = f.read()

        self.filename = os.path.basename(file_path)
        self.filesize = len(self.file_data)

        # ヘッダシグネチャの抽出
        header_bytes = self.file_data[0:24] if self.filesize >= 24 else b""
        try:
            header_str = header_bytes.split(b'\x00')[0].decode('ascii', errors='ignore')
        except Exception:
            header_str = ""

        self.is_cmt_mode = False
        self.is_cas_mode = False
        self.tags_info = []

        if header_str == "PC-8801 Tape Image(T88)":
            self._parse_t88_tags()
        elif b"\x1F\xA6\xDE\xBA\xCC\x13\x7D\x74" in self.file_data:
            # 拡張子に依存せず、MSX/PC-6001系のマジックナンバーが存在すればCASとして解析
            self.is_cas_mode = True
            self._parse_cas_tags()
        else:
            # T88でもCASでもない場合はRawベタデータ(CMT)とする
            self.is_cmt_mode = True

    def _parse_cas_tags(self):
        """CAS (MSX/PC-6001等) 専用のブロックパース"""
        self.tags_info = []
        magic = b"\x1F\xA6\xDE\xBA\xCC\x13\x7D\x74"
        
        magic_offsets = []
        idx = self.file_data.find(magic)
        while idx != -1:
            magic_offsets.append(idx)
            idx = self.file_data.find(magic, idx + 8)
            
        if not magic_offsets:
            self.is_cas_mode = False
            self.is_cmt_mode = True
            return

        for i, start_offset in enumerate(magic_offsets):
            end_offset = magic_offsets[i+1] if i + 1 < len(magic_offsets) else self.filesize
            total_len = end_offset - start_offset
            
            data_start_offset = start_offset + 8
            data_len = total_len - 8
            tag_data = self.file_data[data_start_offset:end_offset]
            
            tag_name = "Data (データ本体)"
            tag_id = 0xCA51  # CASデータブロックを表す仮想ID
            
            # CASファイルのヘッダブロックの判定
            if len(tag_data) >= 16:
                type_bytes = tag_data[0:10]
                block_type = ""
                if type_bytes == b"\xD3" * 10:
                    block_type = "BASIC"
                elif type_bytes == b"\xD0" * 10:
                    block_type = "Binary (マシン語)"
                elif type_bytes == b"\xEA" * 10:
                    block_type = "ASCII (データ)"
                
                if block_type:
                    file_name_bytes = tag_data[10:16]
                    file_name = file_name_bytes.decode('ascii', errors='ignore').strip()
                    tag_name = f"Header [{block_type}: {file_name}]"
                    tag_id = 0xCA50  # CASヘッダブロックを表す仮想ID
            
            # 物理再生時間の概算推測 (MSX: 1200bps時、1バイト約8.33ms)
            baud_rate = "1200 bps"
            duration_ms = total_len * (1000.0 / 1200.0) * 11
            duration_ticks = int((duration_ms / 1000.0) * 4800.0) # T88の互換Ticks(4.8kHz)に換算
            
            start_tick = 0
            if self.tags_info:
                prev = self.tags_info[-1]
                start_tick = prev["start_tick"] + prev["duration_ticks"]
            
            self.tags_info.append({
                "no": i,
                "offset": start_offset,
                "header_len": 8,  # CAS特有のヘッダサイズ
                "tag_id": tag_id,
                "tag_name": tag_name,
                "data_len": data_len,
                "total_len": total_len,
                "start_tick": start_tick,
                "duration_ticks": duration_ticks,
                "ms": duration_ms,
                "pure_bytes": f"{data_len:,}",
                "baud_rate": baud_rate,
                "is_cas": True
            })

    def _parse_t88_tags(self):
        """T88専用のタグ構造パース"""
        self.tags_info = []
        offset = 24
        tag_count = 0
        file_length = self.filesize
        TICK_FREQ = 4800.0

        while offset < file_length:
            if file_length - offset < 4:
                break

            tag_id, data_len = struct.unpack_from("<HH", self.file_data, offset)
            tag_name = self.TAG_NAMES.get(tag_id, "Unknown (未知のタグ)")

            total_tag_len = 4 + data_len
            if offset + total_tag_len > file_length:
                tag_name += " [破損:データ不足]"
                total_tag_len = file_length - offset

            start_tick = 0
            tag_duration_ticks = 0
            pure_data_bytes_count = "-"
            baud_rate = ""

            data_start_offset = offset + 4
            tag_data = self.file_data[data_start_offset: data_start_offset + (total_tag_len - 4)]

            if tag_id in (0x0100, 0x0102, 0x0103):
                if len(tag_data) >= 8:
                    start_tick = struct.unpack_from("<L", tag_data, 0)[0]
                    tag_duration_ticks = struct.unpack_from("<L", tag_data, 4)[0]
            elif tag_id == 0x0101:
                pure_data_bytes_count = f"{data_len:,}"
                
                # ★修正: データバイトの単純合算ではなくデータ量からTicksを計算する
                # 1200bps (1bit=4Ticks), 1バイト=11bit (スタート1+データ8+ストップ2) の場合=44Ticks
                tag_duration_ticks = data_len * 44

                if self.tags_info:
                    prev = self.tags_info[-1]
                    start_tick = prev["start_tick"] + prev["duration_ticks"]
                else:
                    start_tick = 0

                if len(tag_data) >= 12:
                    data_type = struct.unpack_from("<H", tag_data, 10)[0]
                    if data_type == 0x01CC:
                        baud_rate = "1200 bps"
                    elif data_type == 0x00CC:
                        baud_rate = "600 bps"
                    else:
                        baud_rate = f"Unknown (0x{data_type:04X})"

            duration_ms = (tag_duration_ticks * 1000.0) / TICK_FREQ if tag_duration_ticks > 0 else 0.0

            self.tags_info.append({
                "no": tag_count,
                "offset": offset,
                "header_len": 4,  # T88の標準ヘッダサイズ
                "tag_id": tag_id,
                "tag_name": tag_name,
                "data_len": data_len,
                "total_len": total_tag_len,
                "start_tick": start_tick,
                "duration_ticks": tag_duration_ticks,
                "ms": duration_ms,
                "pure_bytes": pure_data_bytes_count,
                "baud_rate": baud_rate
            })

            offset += total_tag_len
            tag_count += 1
            if tag_id == 0x0000:
                break

    def generate_hex_dump_string(self, data):
        """バイナリデータを標準的な16進数形式文字列に整形"""
        lines = ["Offset    00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  Decoded Text", "-" * 75]
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            hex_parts = [f"{b:02X}" for b in chunk]
            hex_str1 = " ".join(hex_parts[:8])
            hex_str2 = " ".join(hex_parts[8:])
            hex_dump = f"{hex_str1:<23}  {hex_str2:<23}"
            ascii_dump = "".join(chr(b) if 32 <= b <= 126 else "." for b in chunk)
            lines.append(f"{i:08X}  {hex_dump}  |{ascii_dump}|")
        return "\n".join(lines)
