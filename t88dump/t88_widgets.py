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
        first_row = None

        for offset in range(start_offset, end_offset):
            if offset >= len(file_data):
                break

            row = (offset // 16) + 3
            byte_pos = offset % 16

            if first_row is None:
                first_row = row

            if byte_pos < 8:
                hex_col_start = 10 + (byte_pos * 3)
            else:
                hex_col_start = 10 + (byte_pos * 3) + 1

            self.tag_add("highlight", f"{row}.{hex_col_start}", f"{row}.{hex_col_start + 2}")
            ascii_col_start = 61 + byte_pos
            self.tag_add("highlight", f"{row}.{ascii_col_start}", f"{row}.{ascii_col_start + 1}")

        if first_row is None:
            first_row = (start_offset // 16) + 3
        self.see(f"{first_row}.0")

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
