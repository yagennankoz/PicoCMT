"""
テープイメージのクロスチェックに使う公開ツールとデータを取得する (PC用)

これらはリポジトリに含めず、このスクリプトで tools/external/<名前>/ に取得する (.gitignore 済み)。
取得元はコミットを固定し、取り出したファイルの中身を SHA-256 で確かめる。

| 名前     | 取得元 (ライセンス)                                       | 使うもの                                   |
|----------|-----------------------------------------------------------|--------------------------------------------|
| xm7      | github.com/yas-sim/xm7-related-tools (MIT)                | t77dec / t772wav / wav2t77、T77 と WAV の実物 |
| castools | github.com/joyrex2001/castools (GPL-2.0)                  | cas2wav / wav2cas (MSX, 1200 / 2400 ボー)    |
| papicom  | github.com/hayatsukikazumi/PapicomTapeConvertor (Apache-2.0) | WAV → P6 (PC-6001, 1200 ボー)            |

xm7 のデータ (data/):
  xm7_save.t77         t77dec/bin/BASIC_I.t77      XM7 で BASIC プログラム (LOOP7) を SAVE した T77
  basic_i.wav          wav2t77/bin/basic_i.zip 内  同じプログラムの音声 (8bit モノラル 44.1kHz)
  basic_i_wav2t77.t77  wav2t77/bin/basic_i.zip 内  basic_i.wav を wav2t77 で変換した T77

使い方:
  python tools/fetch_tools.py [名前 ...] [--build]
    名前を省くと全部。--build でツールもビルドする (xm7・castools は gcc / g++、papicom は javac が必要)
  例: python tools/fetch_tools.py xm7            T77 のテスト (pio test) 用のデータだけ取得
      python tools/fetch_tools.py --build        全部取得してビルド (tools/crosscheck.py 用)

Python の標準ライブラリだけで動く。
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTERNAL_DIR = os.path.join(PROJECT_DIR, "tools", "external")
EXE = ".exe" if os.name == "nt" else ""


class Package:
    def __init__(self, name, repo, commit, include, tree_sha256, tools, data=None):
        self.name = name
        self.repo = repo
        self.commit = commit
        self.include = tuple(include)  # 取り出すパス (アーカイブ内、先頭一致)
        self.tree_sha256 = tree_sha256  # 取り出したファイル全体のハッシュ (tree_hash())
        self.tools = tools  # 名前 → ビルド方法
        self.data = data or {}  # 保存名 → (ソース内のパス, zip の中ならその名前, SHA-256)
        self.base = os.path.join(EXTERNAL_DIR, name)
        self.src = os.path.join(self.base, "src")
        self.bin = os.path.join(self.base, "bin")
        self.data_dir = os.path.join(self.base, "data")

    def tool_path(self, tool):
        """ビルドしたツールのパス (papicom は classes フォルダ)"""
        return os.path.join(self.bin, tool if tool == "classes" else tool + EXE)


PACKAGES = {
    "xm7": Package(
        "xm7", "yas-sim/xm7-related-tools", "61d6916aa1558ca1e5e34ad353ea67508ca8354e",
        ["t77dec/", "t772wav/", "wav2t77/", "LICENSE.md"],
        "d935a668f3ee80350445d2f44b3a0fa78d116834595cd9698e402b636d185ce5",
        {t: ("g++", f"{t}/*.cpp") for t in ("t77dec", "t772wav", "wav2t77")},
        {
            "xm7_save.t77": ("t77dec/bin/BASIC_I.t77", None,
                             "7e7aaba3dfbc0228954caa52d2e7ba07ca08ad774f0468eebfebe62526a692fe"),
            "basic_i.wav": ("wav2t77/bin/basic_i.zip", "BASIC_I.wav",
                            "52fe96650d0662450c5057f85fdae33eef623a7bfb54b3f18da65669db05f011"),
            "basic_i_wav2t77.t77": ("wav2t77/bin/basic_i.zip", "basic_i.t77",
                                    "f3d84b984d1224452c1feb2e57e8040d2e8eee82103c93cd151d36cad1a5ed19"),
        }),
    "castools": Package(
        "castools", "joyrex2001/castools", "f505ef64c83ddfabcb6e07ee0422850e085a25d3",
        ["cas2wav.c", "wav2cas.c", "COPYING", "README.md"],
        "b93b947222eefa1ba794f59626f411879fa703966c3cbe43c00bd0f2a2e09569",
        {t: ("gcc", f"{t}.c") for t in ("cas2wav", "wav2cas")}),
    "papicom": Package(
        "papicom", "hayatsukikazumi/PapicomTapeConvertor", "80d212004e3eb3a5870b874fe9b2e6d974149b40",
        ["src/main/java/", "LICENSE", "README.md"],
        "f4f19f48d9746b3ef38f827e9b3b9e371cd1f8b70f8b6f1547e74611c5afeea1",
        {"classes": ("javac", "src/main/java/com/hayatsukikazumi/ptc/*.java")}),
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def tree_hash(root):
    items = []
    for dirpath, _, files in os.walk(root):
        for f in files:
            p = os.path.join(dirpath, f)
            rel = os.path.relpath(p, root).replace(os.sep, "/")
            with open(p, "rb") as fp:
                items.append(f"{rel}:{sha256(fp.read())}")
    items.sort()
    return sha256("\n".join(items).encode())


def download(pkg):
    """必要なファイルだけをアーカイブから展開し、中身を確かめる"""
    if os.path.isdir(pkg.src):
        if tree_hash(pkg.src) == pkg.tree_sha256:
            return
        shutil.rmtree(pkg.src)
    url = f"https://codeload.github.com/{pkg.repo}/zip/{pkg.commit}"
    print(f"ダウンロード: {url}")
    with urllib.request.urlopen(url, timeout=60) as r:
        archive = zipfile.ZipFile(io.BytesIO(r.read()))
    root = archive.namelist()[0].split("/")[0] + "/"
    tmp = pkg.src + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    for info in archive.infolist():
        rel = info.filename[len(root):]
        if info.is_dir() or not rel.startswith(pkg.include):
            continue
        dest = os.path.normpath(os.path.join(tmp, rel))
        if not dest.startswith(os.path.normpath(tmp) + os.sep):
            raise RuntimeError(f"不正なパス: {info.filename}")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(archive.read(info))
    if tree_hash(tmp) != pkg.tree_sha256:
        shutil.rmtree(tmp)
        raise RuntimeError(f"{pkg.name}: 取得したファイルの SHA-256 が一致しません (取得元の内容が変わっている)")
    os.replace(tmp, pkg.src)


def data_ready(pkg):
    for name, (_, _, digest) in pkg.data.items():
        path = os.path.join(pkg.data_dir, name)
        if not os.path.isfile(path):
            return False
        with open(path, "rb") as f:
            if sha256(f.read()) != digest:
                return False
    return True


def extract_data(pkg):
    os.makedirs(pkg.data_dir, exist_ok=True)
    for name, (src, member, digest) in pkg.data.items():
        with open(os.path.join(pkg.src, src), "rb") as f:
            data = f.read()
        if member is not None:
            data = zipfile.ZipFile(io.BytesIO(data)).read(member)
        if sha256(data) != digest:
            raise RuntimeError(f"{name}: SHA-256 が一致しません")
        with open(os.path.join(pkg.data_dir, name), "wb") as f:
            f.write(data)
        print(f"データ: {pkg.name}/data/{name} ({len(data):,} バイト)")


def tools_ready(pkg):
    return all(os.path.exists(pkg.tool_path(t)) for t in pkg.tools)


def find_compiler(name):
    path = shutil.which(name)
    if path is None and os.name == "nt":
        cand = os.path.join(r"C:\msys64\mingw64\bin", name + ".exe")
        path = cand if os.path.isfile(cand) else None
    if path is None:
        raise RuntimeError(f"{name} が見つかりません")
    return path


def build(pkg):
    os.makedirs(pkg.bin, exist_ok=True)
    for tool, (compiler, pattern) in pkg.tools.items():
        exe = find_compiler(compiler)
        src_dir = os.path.join(pkg.src, os.path.dirname(pattern))
        if "*" in pattern:
            suffix = os.path.splitext(pattern)[1]
            srcs = sorted(os.path.join(src_dir, f) for f in os.listdir(src_dir) if f.endswith(suffix))
        else:
            srcs = [os.path.join(pkg.src, pattern)]
        out = pkg.tool_path(tool)
        if compiler == "javac":
            cmd = [exe, "-encoding", "UTF-8", "-nowarn", "-d", out, *srcs]
        elif compiler == "gcc":
            cmd = [exe, "-O2", "-w", "-DBIGENDIAN=0", *srcs, "-o", out, "-lm"]
        else:
            cmd = [exe, "-O2", "-w", "-std=c++17", *srcs, "-o", out]
        env = dict(os.environ)
        env["PATH"] = os.path.dirname(exe) + os.pathsep + env.get("PATH", "")
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if r.returncode != 0:
            raise RuntimeError(f"{tool} のビルドに失敗しました\n{r.stderr[-2000:]}")
        print(f"ビルド: {out}")


def ensure(name, build_tools=False):
    """取得 (と、build_tools のときはビルド) を行う。済んでいれば何もしない"""
    pkg = PACKAGES[name]
    if data_ready(pkg) and (not build_tools or tools_ready(pkg)):
        return pkg
    download(pkg)
    if not data_ready(pkg):
        extract_data(pkg)
    if build_tools and not tools_ready(pkg):
        build(pkg)
    return pkg


def main(argv):
    args = argv[1:]
    build_tools = "--build" in args
    names = [a for a in args if not a.startswith("-")] or list(PACKAGES)
    for n in names:
        if n not in PACKAGES:
            print(f"不明な名前: {n} (選べるもの: {', '.join(PACKAGES)})")
            return 2
    try:
        for n in names:
            ensure(n, build_tools)
            print(f"準備できました: {PACKAGES[n].base}")
    except Exception as e:  # noqa: BLE001
        print(f"エラー: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
