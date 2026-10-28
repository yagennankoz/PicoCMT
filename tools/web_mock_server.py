"""
Webファイルマネージャの確認用サーバー (PC用)

実機に書き込まずに、Webファイルマネージャの画面をPCのブラウザで確認するためのサーバー。
- 画面: include/cmt_web_page.h の WEB_INDEX_HTML をそのまま返す (画面を直したら再起動するだけでよい)
- API : src/cmt_web.cpp と同じ決まりで動く。PC上のフォルダをSDカードに見立てる
- 認証: 本体と同じ Basic 認証 (ID: pico)。--no-auth で認証なし

使い方:
  python tools/web_mock_server.py [SDカードに見立てるフォルダ] [--port 8080]
                                  [--password pico] [--no-auth] [--delay-ms 0]
  ブラウザで http://localhost:8080/ を開く

Python の標準ライブラリだけで動く (3.8 以降)。
"""
import argparse
import base64
import email.parser
import email.policy
import json
import os
import re
import sys
import time
import urllib.parse
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, HTTPServer

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE_HEADER = os.path.join(PROJECT_DIR, "include", "cmt_web_page.h")

WEB_USERNAME = "pico"
NAME_MAX_LEN = 100
INVALID_NAME_CHARS = set('/\\:*?"<>|')


def load_index_html():
    """cmt_web_page.h から WEB_INDEX_HTML の中身を取り出す"""
    with open(PAGE_HEADER, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r'R"rawliteral\((.*)\)rawliteral"', text, flags=re.S)
    if not m:
        raise RuntimeError("WEB_INDEX_HTML not found in " + PAGE_HEADER)
    return m.group(1)


# ----------------------------------------------------------------------------
# src/cmt_web_util.cpp と同じ検証
# ----------------------------------------------------------------------------
def normalize_path(path):
    """パスを正規化する。不正なら None"""
    if not path or not path.startswith("/"):
        return None
    parts = []
    for seg in path.split("/"):
        if seg in ("", "."):
            continue
        if seg == "..":
            return None
        if "\\" in seg or any(ord(c) < 0x20 for c in seg):
            return None
        parts.append(seg)
    return "/" + "/".join(parts)


def is_valid_name(name):
    if not name or len(name.encode("utf-8")) > NAME_MAX_LEN or name in (".", ".."):
        return False
    if any(ord(c) < 0x20 or c in INVALID_NAME_CHARS for c in name):
        return False
    return not name.endswith((" ", "."))


def join_path(directory, name):
    return "/" + name if directory == "/" else directory + "/" + name


def is_hidden(name):
    return name.startswith(".") or name.lower() == "system volume information"


# ----------------------------------------------------------------------------
# HTTP
# ----------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    server_version = "PicoCmtMock/1.0"

    # --- 共通 ---
    def local(self, path):
        return os.path.join(self.server.root, *path.strip("/").split("/")) if path != "/" else self.server.root

    def send_text(self, code, text):
        body = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def check_auth(self):
        if self.server.password is None:
            return True
        expected = "Basic " + base64.b64encode(
            f"{WEB_USERNAME}:{self.server.password}".encode()).decode()
        if self.headers.get("Authorization") == expected:
            return True
        self.send_response(HTTPStatus.UNAUTHORIZED)
        self.send_header("WWW-Authenticate", 'Basic realm="PicoCmt"')
        self.send_header("Content-Length", "0")
        self.end_headers()
        return False

    def arg_path(self, query, key):
        values = query.get(key)
        return normalize_path(values[0]) if values else None

    def route(self, method):
        if self.server.delay_ms:
            time.sleep(self.server.delay_ms / 1000)
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query, keep_blank_values=True)
        routes = {
            ("GET", "/"): self.handle_root,
            ("GET", "/api/list"): self.handle_list,
            ("GET", "/api/download"): self.handle_download,
            ("POST", "/api/upload"): self.handle_upload,
            ("POST", "/api/delete"): self.handle_delete,
            ("POST", "/api/rename"): self.handle_rename,
            ("POST", "/api/mkdir"): self.handle_mkdir,
        }
        handler = routes.get((method, url.path))
        if handler is None:
            self.discard_body()
            self.send_text(404, "Not found")
            return
        handler(query)

    def discard_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def log_message(self, fmt, *args):
        sys.stderr.write("[WEB] %s\n" % (fmt % args))

    # --- GET / ---
    def handle_root(self, query):
        if not self.check_auth():
            return
        body = load_index_html().encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # --- GET /api/list?path= ---
    def handle_list(self, query):
        if not self.check_auth():
            return
        path = self.arg_path(query, "path")
        if path is None:
            self.send_text(400, "Invalid path")
            return
        local = self.local(path)
        if not os.path.isdir(local):
            self.send_text(404, "Folder not found")
            return
        entries = []
        for name in os.listdir(local):
            if is_hidden(name):
                continue
            st = os.stat(os.path.join(local, name))
            is_dir = os.path.isdir(os.path.join(local, name))
            entries.append({"name": name, "dir": is_dir, "size": 0 if is_dir else st.st_size,
                            "mtime": int(st.st_mtime)})
        body = json.dumps({"path": path, "entries": entries}, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # --- GET /api/download?path= ---
    def handle_download(self, query):
        if not self.check_auth():
            return
        path = self.arg_path(query, "path")
        if path is None:
            self.send_text(400, "Invalid path")
            return
        local = self.local(path)
        if not os.path.isfile(local):
            self.send_text(404, "File not found")
            return
        name = path.rsplit("/", 1)[-1]
        ascii_name = "".join(c if ord(c) < 0x80 and c != '"' else "_" for c in name)
        with open(local, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Disposition",
                         f'attachment; filename="{ascii_name}"; '
                         f"filename*=UTF-8''{urllib.parse.quote(name, safe='')}")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # --- POST /api/upload?dir=  (multipart、1リクエストに1ファイル) ---
    def handle_upload(self, query):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        if not self.check_auth():
            return
        directory = self.arg_path(query, "dir")
        if directory is None or not os.path.isdir(self.local(directory)):
            self.send_text(400, "Invalid folder")
            return
        content_type = self.headers.get("Content-Type", "")
        msg = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(
            b"Content-Type: " + content_type.encode() + b"\r\n\r\n" + body)
        files = [part for part in msg.iter_parts() if part.get_filename()] if msg.is_multipart() else []
        if not files:
            self.send_text(400, "No file")
            return
        part = files[0]
        name = re.split(r"[/\\]", part.get_filename())[-1]
        if not is_valid_name(name):
            self.send_text(400, "Invalid file name")
            return
        final = self.local(join_path(directory, name))
        temp = final + ".part"
        with open(temp, "wb") as f:
            f.write(part.get_payload(decode=True) or b"")
        os.replace(temp, final)
        self.send_text(200, "OK")

    # --- POST /api/delete?path= ---
    def handle_delete(self, query):
        self.discard_body()
        if not self.check_auth():
            return
        path = self.arg_path(query, "path")
        if path is None or path == "/":
            self.send_text(400, "Invalid path")
            return
        local = self.local(path)
        if not os.path.exists(local):
            self.send_text(404, "Not found")
            return
        try:
            if os.path.isdir(local):
                os.rmdir(local)
            else:
                os.remove(local)
        except OSError:
            self.send_text(409, "Cannot delete folder (not empty?)" if os.path.isdir(local)
                           else "Cannot delete file")
            return
        self.send_text(200, "OK")

    # --- POST /api/rename?path=&name= ---
    def handle_rename(self, query):
        self.discard_body()
        if not self.check_auth():
            return
        path = self.arg_path(query, "path")
        if path is None or path == "/":
            self.send_text(400, "Invalid path")
            return
        name = (query.get("name") or [""])[0]
        if not is_valid_name(name):
            self.send_text(400, "Invalid name")
            return
        local = self.local(path)
        new_local = os.path.join(os.path.dirname(local), name)
        if not os.path.exists(local):
            self.send_text(404, "Not found")
            return
        if os.path.exists(new_local):
            self.send_text(409, "The name is already used")
            return
        os.rename(local, new_local)
        self.send_text(200, "OK")

    # --- POST /api/mkdir?path=&name= ---
    def handle_mkdir(self, query):
        self.discard_body()
        if not self.check_auth():
            return
        parent = self.arg_path(query, "path")
        if parent is None or not os.path.isdir(self.local(parent)):
            self.send_text(400, "Invalid path")
            return
        name = (query.get("name") or [""])[0]
        if not is_valid_name(name):
            self.send_text(400, "Invalid name")
            return
        new_local = self.local(join_path(parent, name))
        if os.path.exists(new_local):
            self.send_text(409, "The name is already used")
            return
        os.mkdir(new_local)
        self.send_text(200, "OK")


def main():
    ap = argparse.ArgumentParser(description="PicoCmt Web file manager mock server")
    ap.add_argument("root", nargs="?", default=os.path.join(PROJECT_DIR, "tools", "web_mock_sd"),
                    help="SDカードに見立てるフォルダ (既定: tools/web_mock_sd)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--password", default="pico", help="Basic認証のパスワード (ID は pico)")
    ap.add_argument("--no-auth", action="store_true", help="認証なしにする")
    ap.add_argument("--delay-ms", type=int, default=0, help="応答を遅らせる (実機の遅さの再現)")
    args = ap.parse_args()

    os.makedirs(args.root, exist_ok=True)
    server = HTTPServer(("127.0.0.1", args.port), Handler)
    server.root = os.path.abspath(args.root)
    server.password = None if args.no_auth else args.password
    server.delay_ms = args.delay_ms

    auth = "none" if args.no_auth else f"{WEB_USERNAME} / {args.password}"
    print(f"PicoCmt Web mock server: http://localhost:{args.port}/")
    print(f"  SD root : {server.root}")
    print(f"  login   : {auth}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
