"""Check the native TCP server, live launcher session and iconless dialogs on a private desktop."""
import argparse
from contextlib import contextmanager, ExitStack
from pathlib import Path
import queue
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
from xml.etree import ElementTree as ET

import paridade as p


@contextmanager
def server(exe):
    with subprocess.Popen([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                          errors="replace", creationflags=subprocess.CREATE_NO_WINDOW) as proc:
        lines = queue.Queue()
        def read_lines():
            for line in proc.stdout:
                lines.put(line.strip())
        reader = threading.Thread(target=read_lines)
        reader.start()

        def until(fragment, seconds=8):
            deadline = time.monotonic() + seconds
            while True:
                try:
                    line = lines.get(timeout=max(0, deadline - time.monotonic()))
                except queue.Empty:
                    raise AssertionError(f"No server output containing {fragment!r}") from None
                if fragment in line:
                    return line
        try:
            until("UpdateServerss TCP")
            yield until
        finally:
            proc.terminate()
            proc.wait(timeout=5)
            reader.join()


def receive(sock, expected):
    def exact(length):
        data = b""
        while len(data) < length:
            block = sock.recv(length - len(data))
            assert block, "unexpected disconnect"
            data += block
        return data
    size, opcode = struct.unpack("<HH", exact(4))
    assert opcode == expected
    return exact(size)


def iconless(hwnd):
    assert hwnd
    children = []
    @p.callback
    def collect(child, _):
        children.append(child)
        return True
    p.api(p.U, "EnumChildWindows", p.W.BOOL, p.H, p.callback, p.W.LPARAM)(hwnd, collect, 0)
    for child in children:
        name = p.C.create_unicode_buffer(64)
        p.api(p.U, "GetClassNameW", p.C.c_int, p.H, p.W.LPWSTR, p.C.c_int)(child, name, len(name))
        style = p.api(p.U, "GetWindowLongW", p.W.LONG, p.H, p.C.c_int)(child, -16)
        assert name.value != "Static" or style & 0x1f != 3  # SS_ICON


def exercise(base, server_binary, desk):
    exe = base / "UpdateServerss.exe"
    shutil.copy2(server_binary, exe)
    release = p.ROOT / "build/client/Release"
    for name in ("PBLauncher.exe", "ResourceCheck.exe", "Messages.xml", "UI.xml", "Launcher.svl"):
        shutil.copy2(release / name, base / name)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    (base / "Config.ini").write_text(f"[Address]\nIp0=127.0.0.1\nPort0={port}\n[HTTP]\nEnabled=0\n")
    (base / "Version.ini").write_text(f"{p.GAME_VERSION}\n{p.DATE}\nhttp://127.0.0.1:{port}/patch\n")
    (base / "UserFileList.dat").write_bytes(p.catalog({}))
    cfg = p.rotated_xml(release / "config.zpt")
    cfg.find("server_address").text = "127.0.0.1"
    cfg.find("server_port").text = str(port)
    notice = cfg.find("notice_ui")
    if notice is None:
        notice = ET.SubElement(cfg, "notice_ui")
    notice.text = "hide"
    (base / "config.zpt").write_bytes(p.encode_xml(cfg))
    request = struct.pack("<HHI8x", 12, 1538, p.GAME_VERSION)
    report = struct.pack("<HHII", 8, 1540, 16, 0x82000017)
    other = base / "outra instalação"
    other.mkdir()
    game_name = cfg.findtext("executable_file")
    (other / game_name).write_bytes(b"file in the shortcut's working directory")

    with ExitStack() as stack:
        first = stack.enter_context(server(exe))
        with ExitStack() as peers:
            for _ in range(12):
                sock = peers.enter_context(socket.create_connection(("127.0.0.1", port), timeout=3))
                assert receive(sock, 1536) == b""
                sock.sendall(request)
                assert struct.unpack_from("<I", receive(sock, 1539))[0] == p.GAME_VERSION
            sock.sendall(request + report)
            assert struct.unpack_from("<I", receive(sock, 1539))[0] == p.GAME_VERSION
            assert receive(sock, 1541) == b""
            first("erro=0x82000017")

        with desk.run(base / "PBLauncher.exe", cwd=other) as launcher:
            main = p.wait(lambda: desk.find(launcher, 201))
            line = first("RX 1538")
            peer = line.split("[")[1].split("]")[0]
            first(f"[{peer}] RX 1538", 22)
            p.click(main, 205)
            question = p.wait(lambda: desk.find(launcher, 7))
            iconless(question)
            p.click(question, 7)
            p.wait(lambda: p.enabled(main))
            stack.close()
            with server(exe) as restarted:
                restarted("RX 1538", 24)
                assert p.enabled(p.child(main, 202))
                p.click(main, 202)
                assert "erro=0x82000017" in restarted("RX 1540")
                error = p.wait(lambda: desk.find(launcher, 2) or desk.find(launcher, 1))
                assert str(base / game_name) in p.caption(p.child(error, 65535))
                iconless(error)
                p.click(error, 2 if p.child(error, 2) else 1)
                p.wait(lambda: p.wait_process(launcher.handle, 0) == 0)
                restarted("desconectado")
    cfg.find("executable_file").text = str(Path(other.name) / game_name)
    (base / "config.zpt").write_bytes(p.encode_xml(cfg))
    with desk.run(base / "PBLauncher.exe", cwd=other) as launcher:
        error = p.wait(lambda: desk.find(launcher, 2) or desk.find(launcher, 1))
        assert "executable_file" in p.caption(p.child(error, 65535))
        p.click(error, 2 if p.child(error, 2) else 1)
        p.wait(lambda: p.wait_process(launcher.handle, 0) == 0)
    with desk.run(base / "ResourceCheck.exe") as helper:
        error = p.wait(lambda: desk.find(helper, 2) or desk.find(helper, 1))
        iconless(error)
        p.click(error, 2 if p.child(error, 2) else 1)
        p.wait(lambda: p.wait_process(helper.handle, 0) == 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, default=p.ROOT / "build/server/Release/UpdateServerss.exe")
    args = parser.parse_args()
    desk = p.Desktop()
    try:
        with tempfile.TemporaryDirectory(prefix="PB-sessions-") as temp:
            exercise(Path(temp), args.server, desk)
    finally:
        desk.switch(desk.previous)
        p.api(p.U, "CloseDesktop", p.W.BOOL, p.H)(desk.handle)
    print("PASS concurrent peers, persistent frames, live logs, idle traffic, reconnect and iconless dialogs")
