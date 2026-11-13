"""Exercise news loading in the Release client with the installed WebView2 runtime."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shutil
import socketserver
import tempfile
import threading
from xml.etree import ElementTree as ET

import paridade as p


class News(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/news":
            self.server.requested.set()
            self.server.release_page.wait(15)
            body = b'<!doctype html><body style="background:#123456">News<img src="/slow-image">'
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
        elif self.path == "/slow-image":
            self.server.image_requested.set()
            self.server.release_image.wait(15)
            self.send_response(204)
            body = b""
        else:
            self.send_error(404)
            return
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def exercise(game, desktop, versions, http):
    release = p.ROOT / "build/client/Release"
    for name in ("PBLauncher.exe", "ResourceCheck.exe", "Launcher.svl", "Messages.xml", "UI.xml"):
        shutil.copy2(release / name, game / name)
    (game / "UserFileList.dat").write_bytes(p.catalog({}))
    config = p.rotated_xml(release / "config.zpt")
    config.find("server_address").text = "127.0.0.1"
    config.find("server_port").text = str(versions.server_address[1])
    notice = config.find("notice_ui")
    if notice is None:
        notice = ET.SubElement(config, "notice_ui")
    url = f"http://127.0.0.1:{http.server_port}/news"

    def configure(address, hidden=False):
        notice.text = "hide" if hidden else "show"
        config.find("web_url").text = address
        (game / "config.zpt").write_bytes(p.encode_xml(config))

    exe = game / "PBLauncher.exe"
    configure(url)
    with desktop.run(exe) as proc:
        main = p.wait(lambda: desktop.find(proc, 201))
        panel = p.child(main, 221)
        assert p.enabled(p.child(main, 202)) and p.caption(panel) == "" and p.visible(panel)
        p.wait(http.requested.is_set)
        http.release_page.set()
        p.wait(http.image_requested.is_set)
        child_window = p.api(p.U, "GetWindow", p.H, p.H, p.W.UINT)
        p.wait(lambda: p.visible(child_window(panel, 5)))  # GW_CHILD, while the image is still pending.
        http.release_image.set()
        p.finish(desktop, proc, main)

    http.requested.clear()
    configure(url, hidden=True)
    with desktop.run(exe) as proc:
        main = p.wait(lambda: desktop.find(proc, 201))
        assert not p.visible(p.child(main, 221)) and not http.requested.is_set()
        p.finish(desktop, proc, main)
    configure("http://127.0.0.1:1/news")
    with desktop.run(exe) as proc:
        main = p.wait(lambda: desktop.find(proc, 201))
        p.wait(lambda: p.caption(p.child(main, 221)) == "Notícias indisponíveis.")
        assert p.enabled(p.child(main, 202))
        p.finish(desktop, proc, main)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="PB-news-", ignore_cleanup_errors=True) as tmp, \
            ThreadingHTTPServer(("127.0.0.1", 0), News) as http, \
            socketserver.ThreadingTCPServer(("127.0.0.1", 0), p.Version) as versions:
        versions.version, versions.base = p.GAME_VERSION, f"http://127.0.0.1:{http.server_port}/patch"
        versions.queries, versions.errors = [], []
        http.requested, http.release_page = threading.Event(), threading.Event()
        http.image_requested, http.release_image = threading.Event(), threading.Event()
        for server in (versions, http):
            threading.Thread(target=server.serve_forever, daemon=True).start()
        os.environ["WEBVIEW2_USER_DATA_FOLDER"] = str(Path(tmp) / "profile")
        desktop = p.Desktop()
        try:
            exercise(Path(tmp), desktop, versions, http)
        finally:
            http.release_page.set()
            http.release_image.set()
            desktop.switch(desktop.previous)
            p.api(p.U, "CloseDesktop", p.W.BOOL, p.H)(desktop.handle)
            for server in (versions, http):
                server.shutdown()
    print("PASS slow news, progressive display, hidden panel and unreachable site")
