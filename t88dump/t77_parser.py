"""
T77 (FM-7 / XM7) テープイメージの解析・チェック

T77 フォーマット:
  - 先頭16バイト : "XM7 TAPE IMAGE 0"
  - 以降 2バイト(ビッグエンディアン)単位のワード列
      bit15   : 信号レベル (1=High, 0=Low)
      bit14-0 : そのレベルが続く時間 (カウント)
  - カウント値 0 のワード (0x0000 / 0x8000) は長さを持たないマーカー

デコードの前提 (FM-7 のカセット信号):
  - 1ビット = 1周期。短い周期 (約2400Hz) = '0'、長い周期 (約1200Hz) = '1'
    (XM7 の出力は 短い半周期 約230us / 長い半周期 約440us)
  - 1ビットあたりの半周期数はファイルから自動推定する (想定は 短2 / 長2)
  - バイトはスタートビット(0) + 8ビット + ストップビット(1...)
  - ビット順(LSB/MSBファースト)と極性は、同期パターン "01 3C" が
    最も多く見つかる組み合わせを自動選択する
  - ブロック = 01 3C [種別] [長さ] [データ x 長さ] [チェックサム]

単体でも実行可能:
  python t77_parser.py <file.t77> [--unit 9]
"""
import bisect
import math
import struct
import sys

T77_SIGNATURE = b"XM7 TAPE IMAGE 0"
T77_HEADER_LEN = 16
DEFAULT_UNIT_US = 9.0  # 1カウントあたりの時間 (XM7・t772wav・wav2t77・PicoCmt)

# 半周期の基準値 (XM7 で SAVE した T77 の実測: 短 25.6 / 長 48.8 カウント x 9us)
NOMINAL_SHORT_US = 230.4
NOMINAL_LONG_US = 439.2
NOMINAL_TOLERANCE_PCT = 6  # これ以上ずれたら警告 (テープの速度のずれを考えて広めにする)
MINOR_ISSUE_COUNT = 2  # ノイズ・位相ずれがこの個数以下なら警告にしない (XM7 の出力にも1個程度ある)

TAG_T77_GAP = 0x7700
TAG_T77_BLOCK = 0x7701
TAG_T77_SIGNAL = 0x7702

BLOCK_TYPE_NAMES = {0x00: "Header", 0x01: "Data", 0xFF: "End"}
FILE_TYPE_NAMES = {0x00: "BASIC", 0x01: "Data", 0x02: "Machine"}
SYNC = (0x01, 0x3C)


def _mean_std(values):
    if not values:
        return 0.0, 0.0
    m = sum(values) / len(values)
    var = sum((v - m) ** 2 for v in values) / len(values)
    return m, math.sqrt(var)


def _mode(hist, default):
    if not hist:
        return default
    return max(hist.items(), key=lambda kv: (kv[1], -kv[0]))[0]


class T77Analysis:
    """T77 ファイルを解析し、構造行(rows)とチェック結果レポート(report)を生成する"""

    def __init__(self, data, unit_us=DEFAULT_UNIT_US):
        self.data = data
        self.unit_us = unit_us
        self.errors = []
        self.warnings = []
        self.infos = []
        self.rows = []

        self.runs = []  # (level, counts, off_start, off_end, t_start)
        self.classes = []  # 'S' / 'L' / 'G'(無音) / 'N'(異常)
        self.markers = []  # (offset, word, t)
        self.total_counts = 0
        self.stats = {}
        self.glitch_offsets = []
        self.phase_offsets = []
        self.blocks = []

        self._analyze()
        self.report = self._build_report()

    # ------------------------------------------------------------------
    # 解析本体
    # ------------------------------------------------------------------
    def _analyze(self):
        d = self.data
        if len(d) < T77_HEADER_LEN or not d.startswith(b"XM7 TAPE IMAGE"):
            self.errors.append("ヘッダ \"XM7 TAPE IMAGE 0\" がありません")
            return
        if d[:T77_HEADER_LEN] != T77_SIGNATURE:
            self.warnings.append(f"ヘッダが標準と異なります: {d[:T77_HEADER_LEN]!r}")

        body_len = len(d) - T77_HEADER_LEN
        if body_len % 2:
            self.warnings.append("データ部が奇数バイトです (末尾の1バイトは無視します)")
        n = body_len // 2
        if n == 0:
            self.errors.append("ヘッダのみでデータがありません")
            return

        words = struct.unpack_from(">%dH" % n, d, T77_HEADER_LEN)
        if words[0] == 0x0000:
            self.infos.append("ヘッダ直後に 0x0000 あり (XM7 が書き出すファイルと同じ形)")
        else:
            self.infos.append("ヘッダ直後に 0x0000 なし")

        self._build_runs(words)
        if not self.runs:
            self.errors.append("長さを持つデータがありません")
            return
        if not self._classify():
            return
        self._decode()

    def _build_runs(self, words):
        """同じレベルが続くワードを1つの半周期(ラン)にまとめる"""
        cur_level = None
        cur_count = 0
        cur_off = 0
        t_start = 0
        t = 0
        for i, w in enumerate(words):
            off = T77_HEADER_LEN + i * 2
            cnt = w & 0x7FFF
            lvl = 1 if (w & 0x8000) else 0
            if cnt == 0:
                self.markers.append((off, w, t))
                continue
            if lvl == cur_level:
                cur_count += cnt
            else:
                if cur_level is not None:
                    self.runs.append((cur_level, cur_count, cur_off, off, t_start))
                cur_level, cur_count, cur_off, t_start = lvl, cnt, off, t
            t += cnt
        if cur_level is not None:
            self.runs.append((cur_level, cur_count, cur_off,
                              T77_HEADER_LEN + len(words) * 2, t_start))
        self.total_counts = t

        mid_markers = [m for m in self.markers if m[0] != T77_HEADER_LEN]
        if mid_markers:
            offs = ", ".join(f"0x{m[0]:08X}" for m in mid_markers[:8])
            more = " ..." if len(mid_markers) > 8 else ""
            self.warnings.append(
                f"データ途中にカウント0のマーカーが {len(mid_markers)} 個あります ({offs}{more})。"
                f"PicoCmt の旧版プレーヤーはここで再生が止まります")

    def _classify(self):
        """半周期を短(S)/長(L)/無音(G)/異常(N)に分類する"""
        u = self.unit_us
        cand = sorted(r[1] for r in self.runs if 50 <= r[1] * u <= 1500)
        if len(cand) < 20:
            self.errors.append("FSK信号と思われる半周期が見つかりません (50〜1500us の範囲が20個未満)")
            return False

        c1 = float(cand[len(cand) * 5 // 100])
        c2 = float(cand[len(cand) * 95 // 100])
        for _ in range(30):
            thr = (c1 + c2) / 2
            a = [x for x in cand if x < thr]
            b = [x for x in cand if x >= thr]
            if not a or not b:
                break
            n1, n2 = sum(a) / len(a), sum(b) / len(b)
            if n1 == c1 and n2 == c2:
                break
            c1, c2 = n1, n2

        if c2 < c1 * 1.5:
            # 1種類しかない (リーダーのみ等)。公称値に近い方として扱う
            center = (c1 + c2) / 2
            if abs(center * u - NOMINAL_SHORT_US) < abs(center * u - NOMINAL_LONG_US):
                c1, c2 = center, center * 2
            else:
                c1, c2 = center / 2, center
            self.warnings.append("半周期の長さが1種類しか見つかりません (短い周期/長い周期の片方のみ)")

        thr = math.sqrt(c1 * c2)
        gap_counts = max(c2 * 3, 2000.0 / u)
        self.stats["center_counts"] = (c1, c2)
        self.stats["threshold_counts"] = thr
        self.stats["gap_counts"] = gap_counts

        for r in self.runs:
            c = r[1]
            if c >= gap_counts:
                self.classes.append('G')
            elif c < c1 * 0.5 or c > c2 * 1.6:
                self.classes.append('N')
                self.glitch_offsets.append(r[2])
            else:
                self.classes.append('S' if c < thr else 'L')

        self._timing_stats()
        return True

    def _timing_stats(self):
        u = self.unit_us
        c1, c2 = self.stats["center_counts"]
        for cls, nominal in (('S', NOMINAL_SHORT_US), ('L', NOMINAL_LONG_US)):
            all_us, hi_us, lo_us = [], [], []
            for r, k in zip(self.runs, self.classes):
                if k != cls:
                    continue
                us = r[1] * u
                all_us.append(us)
                (hi_us if r[0] else lo_us).append(us)
            m, sd = _mean_std(all_us)
            self.stats[cls] = {
                "count": len(all_us), "mean": m, "std": sd,
                "min": min(all_us) if all_us else 0, "max": max(all_us) if all_us else 0,
                "mean_high": _mean_std(hi_us)[0], "mean_low": _mean_std(lo_us)[0],
                "dev": (m / nominal - 1) * 100 if m else 0.0,
            }
            st = self.stats[cls]
            label = "短('0')" if cls == 'S' else "長('1')"
            if st["count"] and abs(st["dev"]) > NOMINAL_TOLERANCE_PCT:
                self.warnings.append(
                    f"{label}半周期の平均 {m:.1f}us が基準 (XM7) の {nominal:.1f}us から {st['dev']:+.1f}% ずれています")
            if st["mean_high"] and st["mean_low"]:
                skew = (st["mean_high"] - st["mean_low"]) / m * 100
                st["skew"] = skew
                if abs(skew) > 15:
                    self.warnings.append(
                        f"{label}半周期の High/Low 長さが {skew:+.0f}% 偏っています "
                        f"(H {st['mean_high']:.0f}us / L {st['mean_low']:.0f}us)。"
                        f"入力レベルやしきい値の問題の可能性があります")

        self.stats["unit_est"] = (NOMINAL_SHORT_US / c1, NOMINAL_LONG_US / c2)
        est = (self.stats["unit_est"][0] + self.stats["unit_est"][1]) / 2
        msg = f"信号から逆算したカウント単位は約 {est:.3f}us です (解析では {self.unit_us:.3f}us を使用)"
        if abs(est / self.unit_us - 1) > 0.01:
            self.warnings.append(msg)
        else:
            self.infos.append(msg)

        if self.glitch_offsets:
            # XM7 の出力にも、ブロックの切れ目などに1個程度ある
            (self.warnings if len(self.glitch_offsets) > MINOR_ISSUE_COUNT else self.infos).append(
                f"範囲外の半周期 (ノイズ/欠け) が {len(self.glitch_offsets)} 個あります")

    # ------------------------------------------------------------------
    # ビット・バイト・ブロックのデコード
    # ------------------------------------------------------------------
    def _segments(self):
        """無音(G)で区切った区間を返す: [('G', idx) | ('SIG', start, end)]"""
        out = []
        start = None
        for i, k in enumerate(self.classes):
            if k == 'G':
                if start is not None:
                    out.append(('SIG', start, i))
                    start = None
                out.append(('G', i))
            elif start is None:
                start = i
        if start is not None:
            out.append(('SIG', start, len(self.classes)))
        return out

    def _groups(self, s, e):
        """同じ種類の半周期の連続をまとめる: (種類, 個数, 先頭ランindex)"""
        groups = []
        i = s
        while i < e:
            k = self.classes[i]
            j = i
            while j < e and self.classes[j] == k:
                j += 1
            groups.append((k, j - i, i))
            i = j
        return groups

    def _bits(self, groups, k_s, k_l):
        bits = []  # (bit, offset, t)
        last = len(groups) - 1
        for gi, (k, n, ridx) in enumerate(groups):
            if k == 'N':
                continue
            per = k_s if k == 'S' else k_l
            nb = int(n / per + 0.5)
            # 区間の先頭・末尾は無音とつながって半端になりやすいので除外
            if nb * per != n and 0 < gi < last:
                self.phase_offsets.append(self.runs[ridx][2])
            bit = 0 if k == 'S' else 1  # FM-7: 短い周期 = '0'、長い周期 = '1'
            for j in range(nb):
                r = self.runs[min(ridx + j * per, ridx + n - 1)]
                bits.append((bit, r[2], r[4]))
        return bits

    @staticmethod
    def _frame_bytes(bits, invert, msb_first):
        out = []  # (value, offset, t, framing_error)
        idle_hist = {}
        one = 0 if invert else 1
        n = len(bits)
        i = 0
        while i + 9 < n:
            if bits[i][0] == one:
                i += 1
                continue
            val = 0
            for j in range(8):
                b = bits[i + 1 + j][0] ^ (1 if invert else 0)
                if msb_first:
                    val = (val << 1) | b
                else:
                    val |= b << j
            stop_ok = bits[i + 9][0] == one
            out.append((val, bits[i][1], bits[i][2], not stop_ok))
            if stop_ok:
                k = i + 9
                while k < n and bits[k][0] == one:
                    k += 1
                idle = min(k - (i + 9), 9)
                idle_hist[idle] = idle_hist.get(idle, 0) + 1
                i = k
            else:
                i += 10
        return out, idle_hist

    @staticmethod
    def _count_sync(byts):
        return sum(1 for a, b in zip(byts, byts[1:]) if (a[0], b[0]) == SYNC)

    def _decode(self):
        segs = self._segments()

        # 1ビットあたりの半周期数を推定
        seg_groups = {}
        hist_s, hist_l = {}, {}
        for seg in segs:
            if seg[0] != 'SIG':
                continue
            g = self._groups(seg[1], seg[2])
            seg_groups[seg[1]] = g
            for k, n, _ in g:
                if n > 24:
                    continue
                if k == 'S':
                    hist_s[n] = hist_s.get(n, 0) + 1
                elif k == 'L':
                    hist_l[n] = hist_l.get(n, 0) + 1
        k_l = _mode(hist_l, 2)
        k_s = _mode(hist_s, k_l)
        if k_s < 1 or k_s > 16:
            k_s = 2
        if k_l < 1 or k_l > 16:
            k_l = 2
        if (k_s, k_l) != (2, 2):
            self.warnings.append(
                f"1ビットあたりの半周期数が 短:{k_s} / 長:{k_l} と推定されました (想定は 2 / 2 = 1ビット1周期)")
        self.stats["k"] = (k_s, k_l)
        bit_us = []
        if self.stats['S']["count"]:
            bit_us.append(self.stats['S']["mean"] * k_s)
        if self.stats['L']["count"]:
            bit_us.append(self.stats['L']["mean"] * k_l)
        if bit_us:
            self.stats["baud"] = 1e6 / (sum(bit_us) / len(bit_us))

        seg_bits = {s: self._bits(g, k_s, k_l) for s, g in seg_groups.items()}
        if self.phase_offsets:
            (self.warnings if len(self.phase_offsets) > MINOR_ISSUE_COUNT else self.infos).append(
                f"半周期数がビット境界に合わない箇所が {len(self.phase_offsets)} 個あります (位相ずれ/欠け)")

        # ビット順・極性の自動判定
        best = None
        for invert in (False, True):
            for msb in (False, True):
                framed = {s: self._frame_bytes(b, invert, msb) for s, b in seg_bits.items()}
                hits = sum(self._count_sync(f[0]) for f in framed.values())
                if best is None or hits > best[0]:
                    best = (hits, invert, msb, framed)
        hits, invert, msb, framed = best
        self.stats["bit_order"] = ("MSB" if msb else "LSB") + "ファースト" + (" / 極性反転" if invert else "")
        self.stats["sync_hits"] = hits
        if hits == 0:
            self.errors.append("同期パターン 01 3C が見つかりません (FM-7 のブロックとして読めません)")
        elif invert or msb:
            self.warnings.append(f"同期パターンは {self.stats['bit_order']} で見つかりました (想定外)")

        idle_total = {}
        for f in framed.values():
            for k, v in f[1].items():
                idle_total[k] = idle_total.get(k, 0) + v
        self.stats["idle_hist"] = idle_total

        frame_errs = sum(1 for f in framed.values() for b in f[0] if b[3])
        self.stats["frame_errors"] = frame_errs
        self.stats["byte_count"] = sum(len(f[0]) for f in framed.values())

        for seg in segs:
            if seg[0] == 'G':
                r = self.runs[seg[1]]
                self._add_row('gap', TAG_T77_GAP, "Gap (無音)", r[2], r[3], r[4], r[4] + r[1])
            else:
                self._rows_for_signal(seg[1], seg[2], framed[seg[1]][0])

        ng = [b for b in self.blocks if not b["ok"]]
        if ng:
            self.errors.append(f"異常のあるブロックが {len(ng)} 個あります (チェックサム不一致/フレーミングエラー/途中切れ)")
        if frame_errs:
            self.warnings.append(f"ストップビット異常 (フレーミングエラー) が {frame_errs} バイトあります")

    def _rows_for_signal(self, s, e, byts):
        seg_off_s, seg_off_e = self.runs[s][2], self.runs[e - 1][3]
        seg_t_s = self.runs[s][4]
        seg_t_e = self.runs[e - 1][4] + self.runs[e - 1][1]

        cur_off, cur_t = seg_off_s, seg_t_s
        cur_idx = 0
        i = 0
        found = False
        while i + 3 < len(byts):
            if (byts[i][0], byts[i + 1][0]) != SYNC:
                i += 1
                continue
            found = True
            btype, ln = byts[i + 2][0], byts[i + 3][0]
            if ln == 0 and self._sum_matches(byts, i, 256):
                ln = 256  # 長さ0を256バイトとして扱う形式の場合
            end = i + 4 + ln  # チェックサムの位置
            leader = sum(1 for b in byts[cur_idx:i] if b[0] == 0xFF)
            if end >= len(byts):
                blk = self._make_block(byts[i:], btype, ln, None, leader, truncated=True)
                self._add_block_row(blk, cur_off, seg_off_e, cur_t, seg_t_e)
                return
            blk = self._make_block(byts[i:end + 1], btype, ln, byts[end][0], leader)
            if end + 1 < len(byts):
                off_e, t_e = byts[end + 1][1], byts[end + 1][2]
            else:
                off_e, t_e = seg_off_e, seg_t_e
            self._add_block_row(blk, cur_off, off_e, cur_t, t_e)
            cur_off, cur_t = off_e, t_e
            cur_idx = end + 1
            i = end + 1

        rest = byts[cur_idx:]
        if not found or rest or cur_off < seg_off_e:
            values = bytes(b[0] for b in rest)
            fe = sum(1 for b in rest if b[3])
            name = "Signal (ブロック未検出)" if not found else "Tail (ブロック後の信号)"
            detail = (f"デコードバイト数  : {len(values):,} (うち 0xFF: {values.count(0xFF):,})\n"
                      f"フレーミングエラー: {fe}\n")
            cls = 'error' if (not found and len(values) > 0) or fe else 'signal'
            self._add_row(cls, TAG_T77_SIGNAL, name, cur_off, seg_off_e, cur_t, seg_t_e,
                          decoded=values, detail=detail)

    @staticmethod
    def _sum_matches(byts, i, ln):
        end = i + 4 + ln
        if end >= len(byts):
            return False
        calc = (byts[i + 2][0] + byts[i + 3][0] + sum(b[0] for b in byts[i + 4:end])) & 0xFF
        return calc == byts[end][0]

    def _make_block(self, bseq, btype, ln, checksum, leader, truncated=False):
        values = bytes(b[0] for b in bseq)
        data = values[4:4 + ln]
        calc = (btype + ln + sum(data)) & 0xFF
        calc_data_only = sum(data) & 0xFF
        fe = sum(1 for b in bseq if b[3])
        blk = {
            "type": btype, "len": ln, "data": data, "raw": values, "leader": leader,
            "checksum": checksum, "calc": calc, "calc_data_only": calc_data_only,
            "frame_errors": fe, "truncated": truncated,
        }
        blk["sum_ok"] = checksum is not None and checksum == calc
        blk["ok"] = blk["sum_ok"] and fe == 0 and not truncated
        self.blocks.append(blk)
        return blk

    def _add_block_row(self, blk, off_s, off_e, t_s, t_e):
        no = len(self.blocks)
        tname = BLOCK_TYPE_NAMES.get(blk["type"], f"Type 0x{blk['type']:02X}")
        name = f"{tname} #{no}"
        extra = ""
        if blk["type"] == 0x00 and len(blk["data"]) >= 10:
            fname = blk["data"][0:8].decode("cp932", errors="replace").rstrip()
            ftype = FILE_TYPE_NAMES.get(blk["data"][8], f"0x{blk['data'][8]:02X}")
            aflag = {0x00: "Binary", 0xFF: "ASCII"}.get(blk["data"][9], f"0x{blk['data'][9]:02X}")
            name = f"{tname} [{ftype}: {fname}]"
            extra = (f"ファイル名        : \"{fname}\"\n"
                     f"ファイル種別      : {ftype} / {aflag}\n")

        if blk["truncated"]:
            name += " 途中切れ"
            sum_text = "- (ブロックが途中で切れています)"
        elif blk["sum_ok"]:
            name += " OK"
            sum_text = f"0x{blk['checksum']:02X} 一致"
        else:
            name += " SUM NG"
            sum_text = f"0x{blk['checksum']:02X} / 計算値 0x{blk['calc']:02X} (種別+長さ+データの和) 不一致"
            if blk["checksum"] == blk["calc_data_only"]:
                sum_text += " ※データのみの和なら一致"
        if blk["frame_errors"] and not blk["truncated"]:
            name += " FE"

        detail = (f"ブロック種別      : 0x{blk['type']:02X} ({tname})\n"
                  f"データ長          : {blk['len']} バイト\n"
                  f"{extra}"
                  f"チェックサム      : {sum_text}\n"
                  f"フレーミングエラー: {blk['frame_errors']}\n"
                  f"直前のリーダー    : 0xFF x {blk['leader']}\n")
        self._add_row('ok' if blk["ok"] else 'error', TAG_T77_BLOCK, name, off_s, off_e, t_s, t_e,
                      decoded=blk["raw"], detail=detail)

    def _add_row(self, cls, tag_id, name, off_s, off_e, t_s, t_e, decoded=None, detail=""):
        g = self._count_in(self.glitch_offsets, off_s, off_e)
        p = self._count_in(self.phase_offsets, off_s, off_e)
        if g or p:
            detail += f"ノイズ/欠け       : {g} 個\n位相ずれ          : {p} 箇所\n"
        dur = t_e - t_s
        self.rows.append({
            "no": len(self.rows),
            "offset": off_s,
            "header_len": 0,
            "tag_id": tag_id,
            "tag_name": name,
            "data_len": off_e - off_s,
            "total_len": off_e - off_s,
            "start_tick": t_s,
            "duration_ticks": dur,
            "ms": dur * self.unit_us / 1000.0,
            "pure_bytes": f"{len(decoded):,}" if decoded is not None else "-",
            "baud_rate": f"{self.stats['baud']:.0f} bps" if self.stats.get("baud") else "",
            "decoded": decoded,
            "detail": detail,
            "row_class": cls,
            "is_t77": True,
        })

    @staticmethod
    def _count_in(sorted_offsets, lo, hi):
        return bisect.bisect_left(sorted_offsets, hi) - bisect.bisect_left(sorted_offsets, lo)

    # ------------------------------------------------------------------
    # レポート
    # ------------------------------------------------------------------
    def verdict(self):
        if self.errors:
            return "NG"
        if self.warnings:
            return "要確認"
        return "OK"

    def _build_report(self):
        u = self.unit_us
        L = []
        L.append(f"■ 判定: {self.verdict()}")
        L.append(f"  (カウント単位 {u:.3f}us として解析)")
        L.append("")
        for title, items in (("エラー", self.errors), ("警告", self.warnings), ("情報", self.infos)):
            if items:
                L.append(f"■ {title}")
                L.extend(f"  ・{m}" for m in items)
                L.append("")

        if self.runs:
            L.append("■ 全体")
            L.append(f"  ファイルサイズ      : {len(self.data):,} バイト")
            L.append(f"  総時間              : {self.total_counts:,} カウント (約 {self.total_counts * u / 1e6:.2f} 秒)")
            L.append(f"  半周期(ラン)数      : {len(self.runs):,}")
            L.append(f"  先頭のレベル        : {'High' if self.runs[0][0] else 'Low'}")
            L.append(f"  マーカー(カウント0) : {len(self.markers)} 個")
            gaps = [r[1] for r, k in zip(self.runs, self.classes) if k == 'G']
            if gaps:
                L.append(f"  無音区間            : {len(gaps)} 個 (最長 {max(gaps) * u / 1000:.0f}ms)")
            L.append("")

        if 'S' in self.stats:
            c1, c2 = self.stats["center_counts"]
            L.append("■ 半周期の統計")
            L.append(f"  中心値 (カウント)   : 短 {c1:.1f} / 長 {c2:.1f} (比 {c2 / c1:.2f}, XM7 1.91)")
            L.append(f"  しきい値            : {self.stats['threshold_counts']:.1f} カウント "
                     f"(無音判定 {self.stats['gap_counts']:.0f} カウント以上)")
            for cls, label, nominal in (('S', "短('0')", NOMINAL_SHORT_US),
                                        ('L', "長('1')", NOMINAL_LONG_US)):
                st = self.stats[cls]
                if not st["count"]:
                    L.append(f"  {label}: なし")
                    continue
                L.append(f"  {label}: {st['count']:,} 個  平均 {st['mean']:.1f}us (基準 {nominal:.1f}us, "
                         f"{st['dev']:+.1f}%)  σ {st['std']:.1f}us  範囲 {st['min']:.0f}〜{st['max']:.0f}us")
                if "skew" in st:
                    L.append(f"      High平均 {st['mean_high']:.1f}us / Low平均 {st['mean_low']:.1f}us "
                             f"(偏り {st['skew']:+.1f}%)")
            ue = self.stats["unit_est"]
            L.append(f"  逆算カウント単位    : 短から {ue[0]:.3f}us / 長から {ue[1]:.3f}us "
                     f"(参考: 9.000 (XM7・PicoCmt) / 9.070 (MAME) / 8.920 (PicoCmt 1.0.2以前))")
            L.append(f"  ノイズ/欠け         : {len(self.glitch_offsets)} 個")
            L.append("")
            L.extend(self._histogram())
            L.append("")

        if "k" in self.stats:
            k_s, k_l = self.stats["k"]
            L.append("■ ビット/バイト")
            L.append(f"  1ビットの半周期数   : '0'=短x{k_s} / '1'=長x{k_l}")
            if self.stats.get("baud"):
                L.append(f"  推定ボーレート      : {self.stats['baud']:.0f} bps")
            L.append(f"  ビット順            : {self.stats['bit_order']}")
            L.append(f"  位相ずれ            : {len(self.phase_offsets)} 箇所")
            L.append(f"  デコードバイト数    : {self.stats['byte_count']:,}")
            L.append(f"  フレーミングエラー  : {self.stats['frame_errors']}")
            ih = self.stats["idle_hist"]
            if ih:
                parts = ", ".join(f"{k if k < 9 else '9+'}:{v}" for k, v in sorted(ih.items()))
                L.append(f"  ストップ+アイドル長 : {parts} (ビット数:回数)")
            L.append(f"  同期 01 3C 検出数   : {self.stats['sync_hits']}")
            L.append("")

        if self.blocks:
            L.append("■ ブロック一覧")
            for i, b in enumerate(self.blocks, 1):
                tname = BLOCK_TYPE_NAMES.get(b["type"], f"0x{b['type']:02X}")
                if b["truncated"]:
                    st = "途中切れ"
                elif b["ok"]:
                    st = "OK"
                else:
                    st = "SUM NG" if not b["sum_ok"] else "FE"
                name = ""
                if b["type"] == 0x00 and len(b["data"]) >= 8:
                    name = " \"" + b["data"][0:8].decode("cp932", errors="replace").rstrip() + "\""
                L.append(f"  #{i:3d} {tname:<6} len={b['len']:3d} leader={b['leader']:4d}  {st}{name}")
            L.append("")
        return "\n".join(L)

    def _histogram(self, bin_us=25, max_us=1000, width=50):
        u = self.unit_us
        bins = [0] * (max_us // bin_us + 1)
        for r, k in zip(self.runs, self.classes):
            if k == 'G':
                continue
            us = r[1] * u
            bins[min(int(us // bin_us), len(bins) - 1)] += 1
        peak = max(bins) or 1
        out = ["■ 半周期ヒストグラム (無音を除く)"]
        for i, n in enumerate(bins):
            if n == 0:
                continue
            label = f"{i * bin_us:4d}-{(i + 1) * bin_us - 1:4d}us" if i < len(bins) - 1 else f"{max_us:4d}us以上 "
            bar = "#" * max(1, int(n * width / peak))
            out.append(f"  {label} {n:8,} {bar}")
        return out


def main(argv):
    args = [a for a in argv[1:]]
    unit = DEFAULT_UNIT_US
    if "--unit" in args:
        i = args.index("--unit")
        unit = float(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        print("usage: python t77_parser.py <file.t77> [--unit 9]")
        return 2
    with open(args[0], "rb") as f:
        data = f.read()
    a = T77Analysis(data, unit)
    print(a.report)
    return 0 if a.verdict() != "NG" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
