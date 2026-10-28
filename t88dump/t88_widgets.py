import tkinter as tk
from tkinter import ttk

class DumpTextArea(tk.Text):
    """16進数ダンプ表示とピンポイントハイライト機能を持つカスタムテキストボックス"""
    def __init__(self, master, **kwargs):
        default_kwargs = {"font": ("Consolas", 10), "wrap": tk.NONE, "bg": "#fafafa"}
        default_kwargs.update(kwargs)
        super().__init__(master, **default_kwargs)
        self.tag_configure("highlight", background="#fff2a3", foreground="#000000")

    def highlight_exact_bytes(self, file_data, start_offset, end_offset):
        """指定されたファイルオフセット範囲の16進数とASCII文字をハイライト"""
        self.tag_remove("highlight", "1.0", tk.END)
        end_offset = min(end_offset, len(file_data))

        def hex_col(byte_pos):
            return 10 + (byte_pos * 3) + (1 if byte_pos >= 8 else 0)

        # 大きな範囲(T77の区間など)でも高速に処理できるよう、1行単位でまとめてタグ付けする
        offset = start_offset
        while offset < end_offset:
            row = (offset // 16) + 3
            first_pos = offset % 16
            last_pos = min(15, first_pos + (end_offset - offset) - 1)

            self.tag_add("highlight", f"{row}.{hex_col(first_pos)}", f"{row}.{hex_col(last_pos) + 2}")
            self.tag_add("highlight", f"{row}.{61 + first_pos}", f"{row}.{61 + last_pos + 1}")
            offset += last_pos - first_pos + 1

        self.see(f"{(start_offset // 16) + 3}.0")

    def calculate_offset_from_cursor(self, cursor_index):
        """クリック位置の行・列番号から、ファイル内絶対オフセット(バイト位置)を算出"""
        try:
            row, col = map(int, cursor_index.split('.'))
        except ValueError:
            return None

        if row < 3:
            return None

        base_offset = (row - 3) * 16
        exact_byte_offset = base_offset

        if 10 <= col <= 57:
            clicked_part = col - 10
            if clicked_part > 25:
                clicked_part -= 1
            byte_idx = clicked_part // 3
            if 0 <= byte_idx < 16:
                exact_byte_offset += byte_idx
                return exact_byte_offset
        elif 61 <= col <= 76:
            exact_byte_offset += (col - 61)
            return exact_byte_offset
        return None
