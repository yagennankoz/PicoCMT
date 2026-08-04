import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from t88_parser import TapeImageParser
from t88_widgets import DumpTextArea

class T88GuiDumper(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("レトロPC テープイメージ アナライザー (T88 / CAS / CMT)")
        self.geometry("1280x780")

        self.parser = TapeImageParser()
        self.is_updating_selection = False

        self.create_widgets()

    def create_widgets(self):
        top_frame = ttk.Frame(self, padding=10)
        top_frame.pack(side=tk.TOP, fill=tk.X)

        self.btn_open = ttk.Button(top_frame, text="ファイルを開く", command=self.open_file)
        self.btn_open.pack(side=tk.LEFT, padx=5)

        self.lbl_file_info = ttk.Label(top_frame, text="ファイルが選択されていません", font=("Segoe UI", 10, "bold"))
        self.lbl_file_info.pack(side=tk.LEFT, padx=15)

        main_paned = ttk.PanedWindow(self, orient=tk.VERTICAL)
        main_paned.pack(expand=True, fill=tk.BOTH, padx=10, pady=5)

        self.notebook = ttk.Notebook(main_paned)
        main_paned.add(self.notebook, weight=4)

        # タブA: T88 / CAS 共通構造解析
        self.tab_t88 = ttk.Frame(self.notebook)
        self.notebook.add(self.tab_t88, text=" T88/CAS 構造解析ビュー ")
        t88_paned = ttk.PanedWindow(self.tab_t88, orient=tk.HORIZONTAL)
        t88_paned.pack(expand=True, fill=tk.BOTH, padx=5, pady=5)

        left_frame = ttk.Frame(t88_paned)
        t88_paned.add(left_frame, weight=3)
        columns = ("no", "offset", "id", "name", "size", "start_tick", "tag_duration_ticks", "duration_ms", "bytes")

        style = ttk.Style()
        def fixed_map(option):
            return [elm for elm in style.map('Treeview', query_opt=option) if elm[:2] != ('!disabled', '!selected')]
        style.map('Treeview', foreground=fixed_map('foreground'), background=fixed_map('background'))

        tree_scroll = ttk.Scrollbar(left_frame, orient=tk.VERTICAL)
        self.tree = ttk.Treeview(left_frame, columns=columns, show="headings", selectmode="browse",
                                 yscrollcommand=tree_scroll.set)
        tree_scroll.config(command=self.tree.yview)

        for col in columns:
            self.tree.heading(col, text=col)
            self.tree.column(col, width=75, anchor=tk.CENTER)

        self.tree.tag_configure("tag_data", background="#70f7a0")
        self.tree.tag_configure("tag_blank", background="#a5a5a5")

        tree_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.tree.pack(side=tk.LEFT, expand=True, fill=tk.BOTH)
        self.tree.bind("<<TreeviewSelect>>", self.on_tag_selected)

        # 右ペイン: タグ単体ダンプ
        right_frame = ttk.Frame(t88_paned)
        t88_paned.add(right_frame, weight=2)
        self.lbl_tag_detail = ttk.Label(right_frame, text="選択されたブロックのデータ部バイナリ")
        self.lbl_tag_detail.pack(anchor=tk.W)
        tag_dump_frame = ttk.Frame(right_frame)
        tag_dump_frame.pack(expand=True, fill=tk.BOTH)
        tag_scroll = ttk.Scrollbar(tag_dump_frame, orient=tk.VERTICAL)
        tag_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        self.txt_tag_dump = DumpTextArea(tag_dump_frame, bg="#fcfcfc")
        self.txt_tag_dump.pack(side=tk.LEFT, expand=True, fill=tk.BOTH)

        self.txt_tag_dump.config(yscrollcommand=tag_scroll.set)
        tag_scroll.config(command=self.txt_tag_dump.yview)

        # タブB: 全体ダンプ
        self.tab_full = ttk.Frame(self.notebook)
        self.notebook.add(self.tab_full, text=" ファイル全体ダンプ (クリック連動) ")

        full_scroll = ttk.Scrollbar(self.tab_full, orient=tk.VERTICAL)
        full_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        self.txt_full_dump = DumpTextArea(self.tab_full)
        self.txt_full_dump.pack(side=tk.LEFT, expand=True, fill=tk.BOTH)

        self.txt_full_dump.config(yscrollcommand=full_scroll.set)
        full_scroll.config(command=self.txt_full_dump.yview)

        self.txt_full_dump.bind("<ButtonRelease-1>", self.on_full_dump_clicked)

        # 4. 下側ステータス解析ペイン
        bottom_frame = ttk.Frame(main_paned, padding=5)
        main_paned.add(bottom_frame, weight=1)
        self.txt_status_analysis = tk.Text(bottom_frame, font=("Consolas", 10), height=7, bg="#f0f4f8", wrap=tk.WORD)
        self.txt_status_analysis.pack(expand=True, fill=tk.BOTH)

    def open_file(self):
        file_path = filedialog.askopenfilename(filetypes=[("Tape Image", "*.t88 *.cmt *.cas"), ("All Files", "*.*")])
        if not file_path: return

        try:
            self.parser.load_file(file_path)
        except Exception as e:
            messagebox.showerror("ファイルエラー", f"失敗:\n{e}")
            return

        self.lbl_file_info.config(text=f"ファイル名: {self.parser.filename} ({self.parser.filesize:,} bytes)")

        for item in self.tree.get_children(): self.tree.delete(item)
        self.txt_tag_dump.delete("1.0", tk.END)
        self.txt_full_dump.delete("1.0", tk.END)
        self.txt_status_analysis.delete("1.0", tk.END)

        self.txt_full_dump.insert(tk.END, self.parser.generate_hex_dump_string(self.parser.file_data))

        if self.parser.is_cmt_mode:
            # 構造が無いCMTファイル
            self.notebook.select(self.tab_full)
            self.lbl_tag_detail.config(text="CMTファイルは構造タグを持っていません")
            self.txt_tag_dump.insert(tk.END, "CMTファイルには構造ブロックはありません。\n全体ダンプタブを参照してください。")
            self.txt_status_analysis.insert(tk.END,
                                            f"ファイル形式: CMT (Raw Cassette Tape Binary)\n"
                                            f"ファイルサイズ: {self.parser.filesize:,} バイト\n"
                                            f"--------------------------------------------------------------------------------\n"
                                            f"仕様解説      : CMTフォーマットは、構造タグを持たない純粋なバイトデータの記録形式です。\n"
                                            f"                全体ダンプの任意のバイトをクリックすると、詳細情報が表示されます。")
        else:
            # T88 または CAS ファイル
            self.notebook.select(self.tab_t88)
            self.lbl_tag_detail.config(text="選択されたブロックのデータ部バイナリ")
            for t in self.parser.tags_info:
                row_tags = ()
                if t.get('is_cas'):
                    row_tags = ("tag_data",) if t['tag_id'] == 0xCA51 else ("tag_blank",)
                else:
                    if t['tag_id'] == 0x0101:
                        row_tags = ("tag_data",)
                    elif t['tag_id'] == 0x0100:
                        row_tags = ("tag_blank",)

                self.tree.insert("", tk.END, values=(
                    t["no"], f"0x{t['offset']:08X}", f"0x{t['tag_id']:04X}", t["tag_name"], f"{t['data_len']} B",
                    f"{t['start_tick']:,}", f"{t['duration_ticks']:,}", f"{t['ms']:.1f}", t["pure_bytes"]), tags=row_tags)

    def on_tag_selected(self, event):
        if self.is_updating_selection or self.parser.is_cmt_mode: return
        selected = self.tree.selection()
        if not selected: return

        tag_idx = int(self.tree.item(selected, "values")[0])
        t_meta = self.parser.tags_info[tag_idx]

        # T88の場合は4バイト、CASの場合は8バイトのヘッダ領域をスキップしてデータのみを抽出
        data_start = t_meta['offset'] + t_meta.get('header_len', 4)
        tag_data = self.parser.file_data[data_start: data_start + t_meta['data_len']]

        self.txt_tag_dump.delete("1.0", tk.END)
        self.txt_tag_dump.insert(tk.END, self.parser.generate_hex_dump_string(tag_data))

        detail_text = f"選択されたブロックのデータ部バイナリ"
        if (t_meta['tag_id'] == 0x0101 or t_meta.get('is_cas')) and t_meta.get('baud_rate'):
            detail_text += f" ｜ ボーレート: {t_meta['baud_rate']}"

        self.lbl_tag_detail.config(text=detail_text)

        self.txt_status_analysis.delete("1.0", tk.END)
        self.txt_status_analysis.insert(tk.END, self._get_tag_status_text(t_meta))

        self.is_updating_selection = True
        self.txt_full_dump.highlight_exact_bytes(self.parser.file_data, t_meta['offset'],
                                                 t_meta['offset'] + t_meta['total_len'])
        self.is_updating_selection = False

    def on_full_dump_clicked(self, event):
        if self.is_updating_selection: return
        offset = self.txt_full_dump.calculate_offset_from_cursor(self.txt_full_dump.index(tk.CURRENT))
        if offset is None or offset >= self.parser.filesize: return

        self.txt_status_analysis.delete("1.0", tk.END)

        if self.parser.is_cmt_mode:
            val = self.parser.file_data[offset]
            self.txt_full_dump.highlight_exact_bytes(self.parser.file_data, offset, offset + 1)
            bin_str = f"{val:08b}"
            analysis = (
                f"▼ 現在選択中のCMT(Raw)ファイル絶対アドレス: 0x{offset:08X} (10進数: {offset:,} バイト目)\n"
                f"データ値      : 0x{val:02X} (10進数: {val} | 2進数ビット表現: {bin_str})\n"
                f"--------------------------------------------------------------------------------\n"
                f"💡 [テープデータ配置パターンのヒント]\n"
                f"  ・0x16 (SYN同期): 通常、ブロックの開始前やピー音（マーク）の後に大量に並びます。\n"
                f"  ・0x3A (':' コロン): PC-88系のファイルヘッダ・データ開始合図です。\n"
                f"  ・0xD3 0xD3... : PC-88系/MSX系のBASICテキスト形式ヘッダ識別子としてよく使われます。\n"
            )
            self.txt_status_analysis.insert(tk.END, analysis)
        else:
            for t in self.parser.tags_info:
                if t['offset'] <= offset < (t['offset'] + t['total_len']):
                    self.is_updating_selection = True
                    items = self.tree.get_children()
                    self.tree.selection_set(items[t['no']])
                    self.tree.see(items[t['no']])
                    self.txt_full_dump.highlight_exact_bytes(self.parser.file_data, t['offset'],
                                                             t['offset'] + t['total_len'])
                    self.is_updating_selection = False
                    self.txt_status_analysis.insert(tk.END, self._get_tag_status_text(t, clicked_offset=offset))
                    break

    def _get_tag_status_text(self, t_meta, clicked_offset=None):
        """タグの持つすべてのメタ情報を整形してテキスト化する"""
        text = (
            f"▼ 選択ブロック情報: 0x{t_meta['tag_id']:04X} ({t_meta['tag_name']}) [タグ番号: {t_meta['no']}]\n"
            f"--------------------------------------------------------------------------------\n"
        )
        if clicked_offset is not None:
            text += f"クリック位置      : 0x{clicked_offset:08X} (10進数: {clicked_offset:,} バイト目)\n"

        text += (
            f"開始オフセット    : 0x{t_meta['offset']:08X} (10進数: {t_meta['offset']:,} バイト目)\n"
            f"ブロック全体サイズ: {t_meta['total_len']:,} バイト\n"
            f"データ部サイズ    : {t_meta['data_len']:,} バイト (管理ヘッダ {t_meta.get('header_len', 4)}バイトを除く)\n"
            f"開始時間 (Ticks)  : {t_meta['start_tick']:,}\n"
            f"継続時間 (Ticks)  : {t_meta['duration_ticks']:,} (約 {t_meta['ms']:.2f} ミリ秒)\n"
        )

        if t_meta['tag_id'] == 0x0101 or t_meta.get('is_cas'):
            text += f"純粋データサイズ  : {t_meta['pure_bytes']} バイト\n"

        if t_meta.get('baud_rate'):
            text += f"ボーレート        : {t_meta['baud_rate']}\n"

        return text

if __name__ == "__main__":
    app = T88GuiDumper()
    app.mainloop()