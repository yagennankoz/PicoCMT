"""
PicoCmt の録音・変換・再生の結果を、公開ツールで読めるか確かめる (PC用)

使い方:
  python tools/crosscheck.py [t77] [cas] [p6] [--work 作業フォルダ]
    フォーマットを省くと全部。公開ツールが無ければ tools/fetch_tools.py で取得・ビルドする
    (gcc / g++ と、p6 は javac・java が必要)

確かめること:
  t77 (FM-7)   : xm7-related-tools (t77dec / t772wav / wav2t77)
    - PicoCmt の T77 (XM7 の信号・実物の WAV・合成信号を録音・変換) → t77dec
    - 同上 → t772wav → wav2t77 → t77dec
    - PicoCmt の再生の波形 → wav2t77 → t77dec
  cas (MSX)    : castools (cas2wav / wav2cas)、1200 / 2400 ボー
    - cas2wav の WAV → PicoCmt で録音・変換
    - PicoCmt の再生の波形 → wav2cas
    - PicoCmt で録音・変換した CAS → cas2wav → wav2cas
  p6 (PC-6001) : PapicomTapeConvertor (WAV → P6)、1200 ボー
    - PicoCmt の再生の波形 → PapicomTapeConvertor
  p6t (PC-6001): PC6001VX と同じ読み方 (Python で再現) と PapicomTapeConvertor
    - PicoCmt で録音・変換した P6T → PC6001VX の読み方
    - PicoCmt の P6T の再生の波形 → PapicomTapeConvertor

エミュレータで作った P6T を PicoCmt に通す (結果をエミュレータで読み込んで確かめる):
  python tools/crosscheck.py p6t-roundtrip <入力.p6t> [出力.p6t]
    入力を PicoCmt で 再生 → 録音 → P6T に変換し、データが一致するか確かめて書き出す
    (出力の既定: <入力>_picocmt.p6t)
  python tools/crosscheck.py p6-roundtrip <入力.p6> [出力.p6]
    素の P6 を PicoCmt の P6 として 再生 (1200ボー) → 録音 → P6 に変換する (出力の既定: <入力>_picocmt.p6)
  python tools/crosscheck.py p6t-append <入力1.p6t> <入力2.p6t> [出力フォルダ]
    PicoCmt の追記処理で結合した pico_append.p6t と、それを 再生 → 録音 → 変換 した
    pico_append_roundtrip.p6t を書き出す

エミュレータで作った T88 / CMT (PC-8001/8801) を PicoCmt に通す:
  python tools/crosscheck.py t88-roundtrip <入力.t88> [出力.t88]
    T88 を 再生 → 録音 → T88 に変換し、DATA タグの中身が一致するか確かめる (出力の既定: <入力>_picocmt.t88)
  python tools/crosscheck.py t88-append <入力1.t88> <入力2.t88> [出力フォルダ]
    PicoCmt の追記処理でつないだ pico_append.t88 と、それを 再生 → 録音 → 変換 した
    pico_append_roundtrip.t88 を書き出す
  python tools/crosscheck.py cmt-roundtrip <入力.cmt> [出力.cmt] [--600]
    CMT を 1200 (--600 で 600) ボーで 再生 → 録音 → CMT に変換する (出力の既定: <入力>_picocmt.cmt)。
    CMT の再生は先頭にダミーの 0x00 x8 を出すので、結果はその分だけ長くなる
  python tools/crosscheck.py cas-roundtrip <入力.cas> [出力.cas] [--2400]
    エミュレータで作った CAS (MSX) を 1200 (--2400 で 2400) ボーで 再生 → 録音 → CAS に変換し、
    元と一致するか確かめる (出力の既定: <入力>_picocmt.cas)。CAS にはボーレートの情報が無い
  python tools/crosscheck.py t88-info <ファイル.t88> ...
    QUASI88 と同じ決まりで T88 を読み、タグの一覧を表示する

PicoCmt 側は pio test -e native を環境変数 XCHECK_OUT_DIR / XCHECK_IN_DIR 付きで動かし、
テストが作ったファイルを受け渡す (test/helpers/crosscheck_utils.h)。
"""
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, TOOLS_DIR)
import fetch_tools  # noqa: E402

PIO = os.path.expanduser(os.path.join("~", ".platformio", "penv", "Scripts", "pio.exe"))
if not os.path.isfile(PIO):
    PIO = shutil.which("pio") or "pio"


def run(cmd, cwd=None, env=None):
    return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def pio_test(suite, out_dir, in_dir=None):
    """PicoCmt のテストを動かし、作ったファイルを out_dir に書き出させる"""
    env = dict(os.environ, XCHECK_OUT_DIR=out_dir)
    if in_dir:
        env["XCHECK_IN_DIR"] = in_dir
    r = run([PIO, "test", "-e", "native", "-f", suite], cwd=PROJECT_DIR, env=env)
    if r.returncode != 0:
        print(r.stdout[-4000:], r.stderr[-2000:])
        raise RuntimeError(f"pio test -f {suite} が失敗しました")


def read(path):
    with open(path, "rb") as f:
        return f.read()


def diff_text(expected, actual):
    if actual is None:
        return "ファイルなし"
    if expected == actual:
        return f"{len(actual)}バイト一致"
    n = next((i for i, (a, b) in enumerate(zip(expected, actual)) if a != b),
             min(len(expected), len(actual)))
    return f"不一致 (期待 {len(expected)}バイト / 結果 {len(actual)}バイト、{n} バイト目から違う)"


class Report:
    def __init__(self):
        self.rows = []

    def add(self, label, ok, detail):
        self.rows.append((label, ok, detail))
        print(f"[{'OK' if ok else 'NG'}] {label}: {detail}")

    def check_bytes(self, label, expected, path):
        actual = read(path) if os.path.isfile(path) else None
        self.add(label, actual == expected, diff_text(expected, actual))


# ----------------------------------------------------------------------------
# T77 (FM-7)
# ----------------------------------------------------------------------------
def check_t77(work, rep):
    pkg = fetch_tools.ensure("xm7", build_tools=True)
    tool = pkg.tool_path
    out = os.path.join(work, "out")
    os.makedirs(out)
    pio_test("test_t77", out)

    def t77dec(t77_path, sub):
        d = os.path.join(work, sub)
        os.makedirs(d)
        shutil.copy(t77_path, os.path.join(d, "in.t77"))
        run([tool("t77dec"), "in.t77"], cwd=d)
        outs = [f for f in os.listdir(d) if f != "in.t77"]
        if len(outs) != 1:
            return None, None
        return os.path.splitext(outs[0])[0], read(os.path.join(d, outs[0]))

    ref_name, ref_data = t77dec(os.path.join(pkg.data_dir, "xm7_save.t77"), "ref")
    sys.path.insert(0, os.path.join(PROJECT_DIR, "t88dump"))
    from t77_parser import T77Analysis
    synth = T77Analysis(read(os.path.join(out, "pico_rec_synth.t77")), 9.0)
    expected = {
        "xm7": (ref_name, ref_data),
        "wav": (ref_name, ref_data),
        "synth": ("GAME    ", b"".join(bytes(b["data"]) for b in synth.blocks if b["type"] == 1)),
    }

    cases = []
    for key in ("xm7", "wav", "synth"):
        t77 = os.path.join(out, f"pico_rec_{key}.t77")
        cases.append((f"録音・変換 ({key}) → t77dec", key, t77))
        wav = os.path.join(work, f"rt_{key}.wav")
        rt = os.path.join(work, f"rt_{key}.t77")
        run([tool("t772wav"), t77, wav])
        run([tool("wav2t77"), wav, rt])
        cases.append((f"録音・変換 ({key}) → t772wav → wav2t77 → t77dec", key, rt))
    for key in ("xm7", "synth"):
        t77 = os.path.join(work, f"play_{key}.t77")
        run([tool("wav2t77"), os.path.join(out, f"pico_play_{key}.wav"), t77])
        cases.append((f"再生 ({key}) → wav2t77 → t77dec", key, t77))

    for i, (label, key, path) in enumerate(cases):
        exp_name, exp_data = expected[key]
        name, data = t77dec(path, f"dec{i}") if os.path.isfile(path) else (None, None)
        ok = name == exp_name and data == exp_data
        detail = f'"{name}" {len(data)}バイト' if data is not None else "読めない"
        rep.add("T77 " + label, ok, detail)


# ----------------------------------------------------------------------------
# CAS (MSX)
# ----------------------------------------------------------------------------
def check_cas(work, rep):
    pkg = fetch_tools.ensure("castools", build_tools=True)
    cas2wav, wav2cas = pkg.tool_path("cas2wav"), pkg.tool_path("wav2cas")
    out, inp = os.path.join(work, "out"), os.path.join(work, "in")
    os.makedirs(out)
    os.makedirs(inp)

    # 1. PicoCmt の再生・録音の結果と、基準の CAS
    pio_test("test_cas", out)
    ref = read(os.path.join(out, "cas_ref.cas"))

    # BASIC (ref)・マシン語 (bin、BSAVE)・アスキー形式 (ascii) の3種類
    kinds = (("", "BASIC", ref), ("_bin", "マシン語", read(os.path.join(out, "cas_bin.cas"))),
             ("_ascii", "アスキー", read(os.path.join(out, "cas_ascii.cas"))))

    # 2. cas2wav の WAV を PicoCmt で録音・変換する
    for suffix, _, _ in kinds:
        src = os.path.join(out, "cas_ref.cas" if not suffix else f"cas{suffix}.cas")
        for baud, opt in ((1200, []), (2400, ["-2"])):
            run([cas2wav, *opt, src, os.path.join(inp, f"cas2wav{suffix}_{baud}.wav")])
    pio_test("test_cas", out, inp)
    for suffix, label, expected in kinds:
        for baud in (1200, 2400):
            rep.check_bytes(f"CAS {baud}ボー {label}: cas2wav → PicoCmt で録音・変換", expected,
                            os.path.join(out, f"pico_from_cas2wav{suffix}_{baud}.cas"))

    # 3. PicoCmt の再生の波形を wav2cas で読む
    #    PicoCmt は最後のブロックの後に約1秒のトレーラー (ピー音) を出す。wav2cas はこれを
    #    データの無いブロックのヘッダとみなし、末尾に 埋め草 + シグネチャ を足すので、それは除いて比べる
    sig = bytes([0x1F, 0xA6, 0xDE, 0xBA, 0xCC, 0x13, 0x7D, 0x74])
    for suffix, label, expected in kinds:
        for baud in (1200, 2400):
            dest = os.path.join(work, f"wav2cas_play{suffix}_{baud}.cas")
            run([wav2cas, os.path.join(out, f"pico_play_cas{suffix}_{baud}.wav"), dest])
            actual = read(dest) if os.path.isfile(dest) else None
            note = ""
            if actual and actual.endswith(sig) and actual.startswith(expected) and \
                    not actual[len(expected):-len(sig)].strip(b"\x00"):
                actual = expected
                note = " (末尾のトレーラーによる空のブロックを除く)"
            rep.add(f"CAS {baud}ボー {label}: PicoCmt で再生 → wav2cas", actual == expected,
                    diff_text(expected, actual) + note)

    # 4. PicoCmt で録音・変換した CAS を cas2wav → wav2cas に通す
    for baud, opt in ((1200, []), (2400, ["-2"])):
        wav = os.path.join(work, f"rt_{baud}.wav")
        dest = os.path.join(work, f"rt_{baud}.cas")
        run([cas2wav, *opt, os.path.join(out, f"pico_rec_cas_{baud}.cas"), wav])
        run([wav2cas, wav, dest])
        rep.check_bytes(f"CAS {baud}ボー: PicoCmt で録音・変換 → cas2wav → wav2cas", ref, dest)


# ----------------------------------------------------------------------------
# P6 (PC-6001)
# ----------------------------------------------------------------------------
def check_p6(work, rep):
    pkg = fetch_tools.ensure("papicom", build_tools=True)
    java = fetch_tools.find_compiler("java")
    out = os.path.join(work, "out")
    os.makedirs(out)
    pio_test("test_cmt", out)
    ref = read(os.path.join(out, "p6_ref.p6"))

    # PicoCmt の再生の波形を PapicomTapeConvertor で読む (先頭はピー音 3秒。1秒読み飛ばす)
    wav = os.path.join(work, "play_p6_1200.wav")
    shutil.copy(os.path.join(out, "pico_play_p6_1200.wav"), wav)
    run([java, "-Djava.awt.headless=true", "-cp", pkg.tool_path("classes"),
         "com.hayatsukikazumi.ptc.P6TapeConvertor", "-s1.0", os.path.basename(wav)], cwd=work)
    rep.check_bytes("P6 1200ボー: PicoCmt で再生 → PapicomTapeConvertor", ref,
                    os.path.join(work, "play_p6_1200.P6"))


# ----------------------------------------------------------------------------
# P6T (PC-6001)
# ----------------------------------------------------------------------------
def read_p6t_like_pc6001vx(data):
    """PC6001VX の cP6T::ReadP6T / cP6DATA::Readff / Readfd と同じ手順で P6T を読む。
    戻り値: (ブロックの一覧, エラー)。ブロックは dict (id, name, baud, stime, ptime, data)"""
    def u16(p):
        return data[p] | (data[p + 1] << 8)

    def u32(p):
        return u16(p) | (u16(p + 2) << 16)

    if len(data) < 4:
        return None, "短すぎる"
    beta = u32(len(data) - 4)
    if beta > len(data) - 4:  # ReadP6T: beta > tellg() ならエラー
        return None, "ベタイメージのサイズが不正"
    p = beta
    if data[p:p + 2] != b"P6":
        return None, "フッタの識別子 P6 が無い"
    p += 2
    _version, count = data[p], data[p + 1]
    p += 5  # ver, dbnum, start, basic, page
    p += 2 + u16(p)  # オートスタートのコマンド
    p += 2 + u16(p)  # 拡張情報
    blocks = []
    for _ in range(count):
        if data[p:p + 2] != b"TI":
            return None, "DATAブロックの識別子 TI が無い"
        p += 2
        blk = {"id": data[p], "name": data[p + 1:p + 17].split(b"\0")[0].decode("ascii", "replace")}
        p += 17
        blk["baud"], blk["stime"], blk["ptime"] = u16(p), u16(p + 2), u16(p + 4)
        offset, size = u32(p + 6), u32(p + 10)
        p += 14
        if offset + size > beta:
            return None, "ブロックのデータがベタイメージの外にある"
        blk["data"] = data[offset:offset + size]  # Readfd
        blocks.append(blk)
    return blocks, None


def check_p6t(work, rep):
    out = os.path.join(work, "out")
    os.makedirs(out)
    pio_test("test_p6t", out)

    # 1. PicoCmt で録音・変換した P6T を、PC6001VX と同じ手順で読む
    blocks, err = read_p6t_like_pc6001vx(read(os.path.join(out, "pico_rec.p6t")))
    ref = read(os.path.join(out, "p6t_rec_ref.bin"))
    if blocks is None:
        rep.add("P6T: 録音・変換 → PC6001VX の読み方", False, err)
    else:
        data = b"".join(b["data"] for b in blocks)
        summary = ", ".join(f'{b["name"].strip() or "-"}/{b["baud"]}/{b["stime"]}ms/{b["ptime"]}ms'
                            for b in blocks)
        ok = data == ref and all(b["baud"] in (600, 1200) for b in blocks)
        rep.add("P6T: 録音・変換 → PC6001VX の読み方", ok,
                f"{diff_text(ref, data)}、{len(blocks)}ブロック (名前/ボー/無音/ピー音: {summary})")

    # 2. PicoCmt で P6T を再生した波形を PapicomTapeConvertor で読む
    pkg = fetch_tools.ensure("papicom", build_tools=True)
    java = fetch_tools.find_compiler("java")
    wav = os.path.join(work, "play_p6t.wav")
    shutil.copy(os.path.join(out, "pico_play_p6t.wav"), wav)
    run([java, "-Djava.awt.headless=true", "-cp", pkg.tool_path("classes"),
         "com.hayatsukikazumi.ptc.P6TapeConvertor", "-s0.5", os.path.basename(wav)], cwd=work)
    rep.check_bytes("P6T 1200ボー: PicoCmt で再生 → PapicomTapeConvertor",
                    read(os.path.join(out, "p6t_play_ref.bin")), os.path.join(work, "play_p6t.P6"))


def describe_p6t(data):
    blocks, err = read_p6t_like_pc6001vx(data)
    if blocks is None:
        return f"  P6T として読めない ({err})。素の P6 として扱われる"
    lines = []
    for i, b in enumerate(blocks):
        lines.append(f"  #{i + 1} ID {b['id']} 名前 {b['name']!r:10} {b['baud']}ボー "
                     f"無音 {b['stime']}ms ピー音 {b['ptime']}ms {len(b['data'])}バイト")
    return "\n".join(lines)


def p6_roundtrip(src_path, dest_path=None):
    """外から渡された P6 を PicoCmt で 再生 → 録音 → 変換 し、結果を書き出す"""
    src_path = os.path.abspath(src_path)
    if dest_path is None:
        base, _ = os.path.splitext(src_path)
        dest_path = base + "_picocmt.p6"
    work = tempfile.mkdtemp(prefix="p6_rt_")
    out, inp = os.path.join(work, "out"), os.path.join(work, "in")
    os.makedirs(out)
    os.makedirs(inp)
    shutil.copy(src_path, os.path.join(inp, "input.p6"))
    print(f"入力: {src_path} ({os.path.getsize(src_path)}バイト)")
    pio_test("test_cmt", out, inp)
    shutil.copy(os.path.join(out, "pico_roundtrip.p6"), dest_path)
    print(f"PicoCmt で再生 → 録音 → 変換: {dest_path} ({os.path.getsize(dest_path)}バイト)")
    print("データは入力と一致しました (pio test で確認)")
    return 0


def p6t_append_cmd(a_path, b_path, dest_dir=None):
    """2つの P6T を PicoCmt の追記処理で結合し、さらに 再生 → 録音 → 変換 した結果も書き出す"""
    a_path, b_path = os.path.abspath(a_path), os.path.abspath(b_path)
    dest_dir = os.path.abspath(dest_dir) if dest_dir else os.path.dirname(a_path)
    work = tempfile.mkdtemp(prefix="p6t_ap_")
    out, inp = os.path.join(work, "out"), os.path.join(work, "in")
    os.makedirs(out)
    os.makedirs(inp)
    shutil.copy(a_path, os.path.join(inp, "append_a.p6t"))
    shutil.copy(b_path, os.path.join(inp, "append_b.p6t"))
    pio_test("test_p6t", out, inp)
    for name in ("pico_append.p6t", "pico_append_roundtrip.p6t"):
        dest = os.path.join(dest_dir, name)
        shutil.copy(os.path.join(out, name), dest)
        print(f"{dest}")
        print(describe_p6t(read(dest)))
    print("\nデータは 入力1 + 入力2 と一致しました (pio test で確認)")
    return 0


def p6t_roundtrip(src_path, dest_path=None):
    """外から渡された P6T を PicoCmt で 再生 → 録音 → 変換 し、結果を書き出す"""
    src_path = os.path.abspath(src_path)
    if dest_path is None:
        base, _ = os.path.splitext(src_path)
        dest_path = base + "_picocmt.p6t"
    work = tempfile.mkdtemp(prefix="p6t_rt_")
    out, inp = os.path.join(work, "out"), os.path.join(work, "in")
    os.makedirs(out)
    os.makedirs(inp)
    shutil.copy(src_path, os.path.join(inp, "input.p6t"))
    print(f"入力: {src_path}")
    print(describe_p6t(read(src_path)))
    pio_test("test_p6t", out, inp)
    shutil.copy(os.path.join(out, "pico_roundtrip.p6t"), dest_path)
    print(f"\nPicoCmt で再生 → 録音 → 変換: {dest_path}")
    print(describe_p6t(read(dest_path)))
    print("\nデータは入力と一致しました (pio test で確認)")
    return 0


T88_SIGNATURE = b"PC-8801 Tape Image(T88)\x00"
T88_TAG_NAMES = {0x0000: "END", 0x0001: "VERSION", 0x0100: "BLANK", 0x0101: "DATA",
                 0x0102: "SPACE", 0x0103: "MARK"}


def read_t88_like_quasi88(data):
    """QUASI88 (pc88main.c の sio_getc) と同じ決まりで T88 を読む。
    DATA は長さ12以上、BLANK / SPACE / MARK は長さちょうど8、END (0x0000) で終わり。
    戻り値: (タグの一覧 [(id, start, len, type, payload)], エラー or None)"""
    if data[:24] != T88_SIGNATURE:
        return None, "シグネチャが違う"
    u16 = lambda p: data[p] | (data[p + 1] << 8)  # noqa: E731
    u32 = lambda p: u16(p) | (u16(p + 2) << 16)  # noqa: E731
    tags, pos = [], 24
    while pos + 4 <= len(data):
        tid, size = u16(pos), u16(pos + 2)
        if tid == 0x0000:
            tags.append((tid, 0, 0, 0, b""))
            return tags, None
        body = pos + 4
        if body + size > len(data):
            return tags, f"タグ 0x{tid:04X} がファイルの終わりを超える"
        if tid == 0x0101:
            if size < 12:
                return tags, "DATA タグが12バイトより短い"
            tags.append((tid, u32(body), u32(body + 4), u16(body + 10), data[body + 12:body + size]))
        elif tid in (0x0100, 0x0102, 0x0103):
            if size != 8:
                return tags, f"タグ 0x{tid:04X} の長さが8でない ({size})"
            tags.append((tid, u32(body), u32(body + 4), 0, b""))
        else:
            tags.append((tid, 0, 0, 0, data[body:body + size]))
        pos = body + size
    return tags, "END タグが無い (XM8 はファイルの後ろの 0 を END とみなす)"


def describe_t88(data):
    tags, err = read_t88_like_quasi88(data)
    if tags is None:
        return f"  T88 として読めない ({err})"
    lines = []
    for tid, start, length, dtype, payload in tags:
        name = T88_TAG_NAMES.get(tid, f"0x{tid:04X}")
        if tid == 0x0101:
            baud = {0x01CC: "1200ボー", 0x00CC: "600ボー"}.get(dtype, f"type 0x{dtype:04X}")
            lines.append(f"  {name:7} {start / 4.8:9.0f}ms {length / 4.8:7.0f}ms {baud} "
                         f"{len(payload)}バイト {describe_cmt_bytes(payload)}")
        elif tid in (0x0100, 0x0102, 0x0103):
            lines.append(f"  {name:7} {start / 4.8:9.0f}ms {length / 4.8:7.0f}ms")
        elif tid != 0x0000:
            lines.append(f"  {name:7} {len(payload)}バイト")
    if err:
        lines.append(f"  !! {err}")
    return "\n".join(lines)


def describe_cmt_bytes(data):
    """PC-8001/8801 のテープのデータの中身の見当 (BASIC のヘッダ D3 x10 + 名前6文字、
    N88-BASIC のモニタの W名前 のヘッダ "$$$" + 名前6文字、マシン語の 3A)"""
    j = next((k for k, b in enumerate(data) if b != 0), None)
    if j is not None and data[j:j + 3] == b"$$$" and j + 9 <= len(data):
        name = data[j + 3:j + 9].decode("ascii", "replace").rstrip("\x00 ")
        rest = data[j + 9:]
        return f"マシン語の名前 {name!r}" + (f" + {describe_cmt_bytes(rest)}" if rest else "")
    names, i = [], 0
    while True:
        i = data.find(b"\xd3" * 10, i)
        if i < 0:
            break
        names.append(data[i + 10:i + 16].decode("ascii", "replace").rstrip("\x00 "))
        i += 16
    if names:
        return "BASIC " + ", ".join(repr(n) for n in names)
    j = next((k for k, b in enumerate(data) if b != 0), None)
    if j is not None and data[j] == 0x3A and j + 3 < len(data):
        return f"マシン語 {data[j + 1]:02X}{data[j + 2]:02X}h〜"
    return ""


def t88_roundtrip(src_path, dest_path=None):
    """外から渡された T88 を PicoCmt で 再生 → 録音 → 変換 し、結果を書き出す"""
    src_path = os.path.abspath(src_path)
    if dest_path is None:
        base, _ = os.path.splitext(src_path)
        dest_path = base + "_picocmt.t88"
    work = tempfile.mkdtemp(prefix="t88_rt_")
    out, inp = os.path.join(work, "out"), os.path.join(work, "in")
    os.makedirs(out)
    os.makedirs(inp)
    shutil.copy(src_path, os.path.join(inp, "input.t88"))
    print(f"入力: {src_path}")
    print(describe_t88(read(src_path)))
    pio_test("test_t88", out, inp)
    shutil.copy(os.path.join(out, "pico_roundtrip.t88"), dest_path)
    print(f"\nPicoCmt で再生 → 録音 → 変換: {dest_path}")
    print(describe_t88(read(dest_path)))
    print("\nDATA タグの中身は入力と一致しました (pio test で確認)")
    return 0


def t88_append_cmd(a_path, b_path, dest_dir=None):
    """2つの T88 を PicoCmt の追記処理でつなぎ、さらに 再生 → 録音 → 変換 した結果も書き出す"""
    a_path, b_path = os.path.abspath(a_path), os.path.abspath(b_path)
    dest_dir = os.path.abspath(dest_dir) if dest_dir else os.path.dirname(a_path)
    work = tempfile.mkdtemp(prefix="t88_ap_")
    out, inp = os.path.join(work, "out"), os.path.join(work, "in")
    os.makedirs(out)
    os.makedirs(inp)
    shutil.copy(a_path, os.path.join(inp, "append_a.t88"))
    shutil.copy(b_path, os.path.join(inp, "append_b.t88"))
    pio_test("test_t88", out, inp)
    for name in ("pico_append.t88", "pico_append_roundtrip.t88"):
        dest = os.path.join(dest_dir, name)
        shutil.copy(os.path.join(out, name), dest)
        print(f"{dest}")
        print(describe_t88(read(dest)))
    print("\nDATA タグの中身は 入力1 + 入力2 と一致しました (pio test で確認)")
    return 0


def cmt_roundtrip(src_path, dest_path=None, baud=1200):
    """外から渡された CMT を PicoCmt で 再生 → 録音 → 変換 し、結果を書き出す"""
    src_path = os.path.abspath(src_path)
    if dest_path is None:
        base, _ = os.path.splitext(src_path)
        dest_path = base + "_picocmt.cmt"
    work = tempfile.mkdtemp(prefix="cmt_rt_")
    out, inp = os.path.join(work, "out"), os.path.join(work, "in")
    os.makedirs(out)
    os.makedirs(inp)
    in_name, out_name = (("input.cmt", "pico_roundtrip.cmt") if baud == 1200
                         else ("input_600.cmt", "pico_roundtrip_600.cmt"))
    shutil.copy(src_path, os.path.join(inp, in_name))
    src = read(src_path)
    print(f"入力: {src_path} ({len(src)}バイト) {describe_cmt_bytes(src)}")
    pio_test("test_cmt", out, inp)
    shutil.copy(os.path.join(out, out_name), dest_path)
    print(f"PicoCmt で再生 ({baud}ボー) → 録音 → 変換: {dest_path} ({os.path.getsize(dest_path)}バイト)")
    print("データは 0x00 x8 (CMT の再生で先頭に付くダミー) + 入力 と一致しました (pio test で確認)")
    return 0


def describe_cas(data):
    """CAS の中のファイルの一覧 (ヘッダ D3 / EA / D0 x10 + 名前6文字)"""
    kinds = {0xD3: "BASIC", 0xEA: "アスキー", 0xD0: "マシン語"}
    sig = bytes([0x1F, 0xA6, 0xDE, 0xBA, 0xCC, 0x13, 0x7D, 0x74])
    out = []
    for blk in data.split(sig)[1:]:
        if len(blk) >= 16 and blk[0] in kinds and blk[:10] == bytes([blk[0]]) * 10:
            out.append(f"{kinds[blk[0]]} {blk[10:16].decode('ascii', 'replace').rstrip()!r}")
    return ", ".join(out)


def cas_roundtrip(src_path, dest_path=None, baud=1200):
    """外から渡された CAS を PicoCmt で 再生 → 録音 → 変換 し、結果を書き出す"""
    src_path = os.path.abspath(src_path)
    if dest_path is None:
        base, _ = os.path.splitext(src_path)
        dest_path = base + "_picocmt.cas"
    work = tempfile.mkdtemp(prefix="cas_rt_")
    out, inp = os.path.join(work, "out"), os.path.join(work, "in")
    os.makedirs(out)
    os.makedirs(inp)
    in_name, out_name = (("input.cas", "pico_roundtrip.cas") if baud == 1200
                         else ("input_2400.cas", "pico_roundtrip_2400.cas"))
    shutil.copy(src_path, os.path.join(inp, in_name))
    src = read(src_path)
    print(f"入力: {src_path} ({len(src)}バイト) {describe_cas(src)}")
    pio_test("test_cas", out, inp)
    shutil.copy(os.path.join(out, out_name), dest_path)
    print(f"PicoCmt で再生 ({baud}ボー) → 録音 → 変換: {dest_path} ({os.path.getsize(dest_path)}バイト)")
    print("入力とまったく同じでした (pio test で確認)")
    return 0


CHECKS = {"t77": check_t77, "cas": check_cas, "p6": check_p6, "p6t": check_p6t}


def main(argv):
    args = argv[1:]
    if "-h" in args or "--help" in args:
        print(__doc__)
        return 0
    if args and args[0] in ("p6t-append", "t88-append"):
        if len(args) < 3:
            print(f"usage: python tools/crosscheck.py {args[0]} <入力1> <入力2> [出力フォルダ]")
            return 2
        func = p6t_append_cmd if args[0] == "p6t-append" else t88_append_cmd
        try:
            return func(args[1], args[2], args[3] if len(args) > 3 else None)
        except Exception as e:  # noqa: BLE001
            print(f"失敗しました: {e}")
            return 1
    if args and args[0] == "t88-info":
        for path in args[1:]:
            print(path)
            print(describe_t88(read(path)))
        return 0
    if args and args[0] == "cas-roundtrip":
        baud = 1200
        if "--2400" in args:
            baud = 2400
            args.remove("--2400")
        if len(args) < 2:
            print("usage: python tools/crosscheck.py cas-roundtrip <入力.cas> [出力.cas] [--2400]")
            return 2
        try:
            return cas_roundtrip(args[1], args[2] if len(args) > 2 else None, baud)
        except Exception as e:  # noqa: BLE001
            print(f"失敗しました: {e}")
            return 1
    if args and args[0] == "cmt-roundtrip":
        baud = 1200
        if "--600" in args:
            baud = 600
            args.remove("--600")
        if len(args) < 2:
            print("usage: python tools/crosscheck.py cmt-roundtrip <入力.cmt> [出力.cmt] [--600]")
            return 2
        try:
            return cmt_roundtrip(args[1], args[2] if len(args) > 2 else None, baud)
        except Exception as e:  # noqa: BLE001
            print(f"失敗しました: {e}")
            return 1
    if args and args[0] in ("p6t-roundtrip", "p6-roundtrip", "t88-roundtrip"):
        if len(args) < 2:
            print(f"usage: python tools/crosscheck.py {args[0]} <入力> [出力]")
            return 2
        func = {"p6t-roundtrip": p6t_roundtrip, "p6-roundtrip": p6_roundtrip,
                "t88-roundtrip": t88_roundtrip}[args[0]]
        try:
            return func(args[1], args[2] if len(args) > 2 else None)
        except Exception as e:  # noqa: BLE001
            print(f"失敗しました: {e}")
            return 1
    work = None
    if "--work" in args:
        i = args.index("--work")
        work = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    names = args or list(CHECKS)
    for n in names:
        if n not in CHECKS:
            print(f"不明なフォーマット: {n} (選べるもの: {', '.join(CHECKS)})")
            return 2
    work = work or tempfile.mkdtemp(prefix="xcheck_")
    if os.path.exists(work):
        shutil.rmtree(work)

    rep = Report()
    for n in names:
        try:
            CHECKS[n](os.path.join(work, n), rep)
        except Exception as e:  # noqa: BLE001
            rep.add(n, False, f"実行できませんでした: {e}")
    ok = sum(1 for r in rep.rows if r[1])
    print(f"\n{ok}/{len(rep.rows)} OK  (作業フォルダ: {work})")
    return 0 if ok == len(rep.rows) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
