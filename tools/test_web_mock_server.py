"""
Webファイルマネージャの確認用サーバー (tools/web_mock_server.py) の動作確認

一時フォルダを SD カードに見立ててサーバーを起動し、各 API を呼んで結果を確かめる。
  python tools/test_web_mock_server.py
"""
import base64
import importlib.util
import json
import os
import sys
import tempfile
import threading
import urllib.error
import urllib.parse
import urllib.request
from http.server import HTTPServer

SERVER_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web_mock_server.py")
spec = importlib.util.spec_from_file_location("mock", SERVER_SCRIPT)
mock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mock)

root = tempfile.mkdtemp(prefix="picocmt_web_")
server = HTTPServer(("127.0.0.1", 0), mock.Handler)
server.root, server.password, server.delay_ms = root, "pico", 0
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
mock.Handler.log_message = lambda *a: None

AUTH = "Basic " + base64.b64encode(b"pico:pico").decode()


def req(method, path, body=None, headers=None, auth=True):
    h = dict(headers or {})
    if auth:
        h["Authorization"] = AUTH
    r = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=body, method=method, headers=h)
    try:
        with urllib.request.urlopen(r) as res:
            return res.status, res.read(), dict(res.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)


def upload(directory, name, data):
    b = "----X"
    body = (f"--{b}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{name}\"\r\n"
            f"Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{b}--\r\n".encode()
    return req("POST", "/api/upload?dir=" + urllib.parse.quote(directory), body,
               {"Content-Type": f"multipart/form-data; boundary={b}"})


q = urllib.parse.quote
results = []


def check(label, cond):
    results.append((label, cond))
    print(("OK  " if cond else "NG  ") + label)


s, body, h = req("GET", "/", auth=False)
check("認証なしでは 401 と WWW-Authenticate", s == 401 and "Basic" in h.get("WWW-Authenticate", ""))
s, body, h = req("GET", "/")
check("画面のHTMLを返す", s == 200 and b"<title>PicoCmt</title>" in body and b"fmtTime" in body)

data = bytes(range(256)) * 10
check("アップロード (バイナリ)", upload("/", "GAME.T88", data)[0] == 200)
check("アップロード (日本語の名前)", upload("/", "ゲーム.T77", b"abc")[0] == 200)
check("不正な名前のアップロードは 400", upload("/", "a:b.T88", b"x")[0] == 400)
check(".part が残っていない", not any(n.endswith(".part") for n in os.listdir(root)))

check("フォルダ作成", req("POST", "/api/mkdir?path=/&name=SUB")[0] == 200)
check("同じ名前のフォルダは 409", req("POST", "/api/mkdir?path=/&name=SUB")[0] == 409)
check("サブフォルダへアップロード", upload("/SUB", "X.CAS", b"123")[0] == 200)

s, body, _ = req("GET", "/api/list?path=/")
j = json.loads(body)
names = {e["name"]: e for e in j["entries"]}
check("一覧: path と entries", j["path"] == "/" and set(names) == {"GAME.T88", "ゲーム.T77", "SUB"})
check("一覧: size / dir / mtime", names["GAME.T88"]["size"] == len(data) and names["SUB"]["dir"]
      and names["GAME.T88"]["mtime"] > 1700000000)
s, body, _ = req("GET", "/api/list?path=" + q("//SUB/./"))
check("一覧: パスの正規化", s == 200 and json.loads(body)["path"] == "/SUB")
check("一覧: .. は 400", req("GET", "/api/list?path=" + q("/SUB/.."))[0] == 400)
check("一覧: ないフォルダは 404", req("GET", "/api/list?path=/NONE")[0] == 404)

s, body, h = req("GET", "/api/download?path=" + q("/GAME.T88"))
check("ダウンロード: 中身が一致", s == 200 and body == data)
check("ダウンロード: Content-Disposition", 'filename="GAME.T88"' in h.get("Content-Disposition", ""))
s, body, h = req("GET", "/api/download?path=" + q("/ゲーム.T77"))
check("ダウンロード: 日本語の名前は filename*", s == 200 and "%E3%82%B2" in h.get("Content-Disposition", ""))

check("リネーム", req("POST", "/api/rename?path=" + q("/GAME.T88") + "&name=NEW.T88")[0] == 200)
check("リネーム: 既存の名前は 409", req("POST", "/api/rename?path=" + q("/NEW.T88") + "&name=SUB")[0] == 409)
check("空でないフォルダの削除は 409", req("POST", "/api/delete?path=/SUB")[0] == 409)
check("ファイルの削除", req("POST", "/api/delete?path=" + q("/SUB/X.CAS"))[0] == 200)
check("空のフォルダの削除", req("POST", "/api/delete?path=/SUB")[0] == 200)
check("ルートの削除は 400", req("POST", "/api/delete?path=/")[0] == 400)
check("上書きアップロード", upload("/", "NEW.T88", b"new")[0] == 200
      and open(os.path.join(root, "NEW.T88"), "rb").read() == b"new")
check("認証なしの API は 401", req("GET", "/api/list?path=/", auth=False)[0] == 401)

server.shutdown()
print(f"\n{sum(c for _, c in results)}/{len(results)} passed")
sys.exit(0 if all(c for _, c in results) else 1)
