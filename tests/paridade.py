"""Run the release client through its normal window and network entry points on a private Windows desktop."""
import ctypes as C
from ctypes import wintypes as W
from contextlib import contextmanager
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import importlib.util
import mmap
import os
from pathlib import Path
import shutil
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import uuid
from xml.etree import ElementTree as ET
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[1]
STAMP = 20160427012554
DATE = "2016-04-27_01-25-54"
GAME_VERSION = 20171214


def archive(name, data):
    stream = io.BytesIO()
    with ZipFile(stream, "w", ZIP_DEFLATED) as z:
        z.writestr(name, data)
    return stream.getvalue()


def catalog(files, stamp=STAMP, fmt=4, *, flags=None, modified=None):
    def words(n):
        return f"{n >> 32} {n & 0xffffffff}"
    rows = ["*Header\t" + words(stamp)]
    for name, data in files.items():
        digest = " ".join(map(str, reversed(struct.unpack("<4I", hashlib.md5(data).digest()))))
        path = Path(name)
        folder = str(path.parent) + "\\"
        fields = ["*FileInfo", f'"{path.name}"', f'"{folder}"', words(STAMP), str(len(data)), digest]
        if fmt >= 2:
            time_value = (modified or {}).get(name, 0)
            fields += ["0 0", f"{time_value & 0xffffffff} {time_value >> 32}"]
        if fmt >= 3:
            fields += ["11"]
        if fmt >= 4:
            fields += [str((flags or {}).get(name, 15))]
        rows.append("\t".join(fields))
    if fmt == 2:
        rows = [row.replace("\t", ";") for row in rows]
    body = (",\b" if fmt == 2 else "\n").join(rows)
    raw = struct.pack("<I", fmt) + (body + "\n\0").encode("cp1252")
    return bytes(((value << 4) | (raw[(i+1) % len(raw)] >> 4)) & 255 for i, value in enumerate(raw))


def wait(fn, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = fn()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError(f"Timeout at line {fn.__code__.co_firstlineno}")


def api(library, name, result, *args):
    fn = getattr(library, name)
    fn.argtypes, fn.restype = args, result
    return fn


K = C.WinDLL("kernel32", use_last_error=True)
U = C.WinDLL("user32", use_last_error=True)
H = W.HANDLE
callback = C.WINFUNCTYPE(W.BOOL, H, W.LPARAM)
child = api(U, "GetDlgItem", H, H, C.c_int)
visible = api(U, "IsWindowVisible", W.BOOL, H)
enabled = api(U, "IsWindowEnabled", W.BOOL, H)
send = api(U, "SendMessageW", C.c_ssize_t, H, W.UINT, W.WPARAM, W.LPARAM)
post = api(U, "PostMessageW", W.BOOL, H, W.UINT, W.WPARAM, W.LPARAM)
close = api(K, "CloseHandle", W.BOOL, H)
wait_process = api(K, "WaitForSingleObject", W.DWORD, H, W.DWORD)


class Startup(C.Structure):
    _fields_ = [("cb", W.DWORD), ("reserved", W.LPWSTR), ("desktop", W.LPWSTR), ("title", W.LPWSTR),
                *[(k, W.DWORD) for k in ("x", "y", "cx", "cy", "cols", "rows", "fill", "flags")],
                ("show", W.WORD), ("cb2", W.WORD), ("reserved2", H), ("stdin", H), ("stdout", H), ("stderr", H)]


class Process(C.Structure):
    _fields_ = [("handle", H), ("thread", H), ("pid", W.DWORD), ("tid", W.DWORD)]


class Desktop:
    def __init__(self):
        self.name = "LauncherParity" + uuid.uuid4().hex
        self.handle = api(U, "CreateDesktopW", H, W.LPCWSTR, H, H, W.DWORD, W.DWORD, H)(
            self.name, None, None, 0, 0x1ff, None)
        self.previous = api(U, "GetThreadDesktop", H, W.DWORD)(api(K, "GetCurrentThreadId", W.DWORD)())
        self.switch = api(U, "SetThreadDesktop", W.BOOL, H)
        if not self.handle or not self.switch(self.handle):
            raise C.WinError(C.get_last_error())

    @contextmanager
    def run(self, exe, *args, cwd=None):
        si, proc = Startup(), Process()
        si.cb, si.desktop = C.sizeof(si), self.name
        line = C.create_unicode_buffer(subprocess.list2cmdline([str(exe), *args]))
        launch = api(K, "CreateProcessW", W.BOOL, W.LPCWSTR, W.LPWSTR, H, H, W.BOOL, W.DWORD, H,
                     W.LPCWSTR, C.POINTER(Startup), C.POINTER(Process))
        if not launch(str(exe), line, None, None, False, 0, None, str(cwd or exe.parent), C.byref(si), C.byref(proc)):
            raise C.WinError(C.get_last_error())
        close(proc.thread)
        try:
            yield proc
        finally:
            if wait_process(proc.handle, 0) != 0:
                api(K, "TerminateProcess", W.BOOL, H, W.UINT)(proc.handle, 2)
                wait_process(proc.handle, 3000)
            close(proc.handle)

    def find(self, proc, control):
        result = []
        @callback
        def visit(h, _):
            pid = W.DWORD()
            api(U, "GetWindowThreadProcessId", W.DWORD, H, C.POINTER(W.DWORD))(h, C.byref(pid))
            if (proc is None or pid.value == proc.pid) and visible(h) and child(h, control):
                result.append(h)
            return True
        api(U, "EnumDesktopWindows", W.BOOL, H, callback, W.LPARAM)(self.handle, visit, 0)
        return result[0] if result else None


def caption(hwnd):
    buf = C.create_unicode_buffer(4096)
    result = C.c_size_t()
    timed = api(U, "SendMessageTimeoutW", C.c_ssize_t, H, W.UINT, W.WPARAM, W.LPARAM, W.UINT, W.UINT,
                C.POINTER(C.c_size_t))
    timed(hwnd, 13, len(buf), C.addressof(buf), 2, 300, C.byref(result))
    return buf.value


def click(main, ident):
    assert post(main, 0x111, ident, child(main, ident))


def exercise_title_buttons(main):
    buttons = [child(main, ident) for ident in (203, 204, 205)]
    get_dc = api(U, "GetDC", H, H)
    release_dc = api(U, "ReleaseDC", C.c_int, H, H)
    pixel = api(C.WinDLL("gdi32"), "GetPixel", W.DWORD, H, C.c_int, C.c_int)
    normal, hover, down = 0x46372f, 0x372b25, 0x2c231e
    center = 12 | (12 << 16)

    def verify(active=None, pressed=False):
        expected = [normal] * len(buttons)
        if active is not None:
            expected[active] = down if pressed else hover
        actual = []
        for button in buttons:
            dc = get_dc(button)
            try:
                actual.append(pixel(dc, 1, 1))
            finally:
                release_dc(button, dc)
        assert actual == expected, ([hex(v) for v in actual], [hex(v) for v in expected])

    # Deliver the new entry before the previous button's queued WM_MOUSELEAVE.
    for order in ((0, 1, 2), (2, 1, 0)):
        previous = None
        for index in order:
            send(buttons[index], 0x0200, 0, center)  # WM_MOUSEMOVE
            verify(index)
            if previous is not None:
                send(buttons[previous], 0x02a3, 0, 0)
                verify(index)
            send(buttons[index], 0x0201, 1, center)  # Press, then drag out to cancel the click.
            verify(index, pressed=True)
            send(buttons[index], 0x0200, 1, 0xffffffff)
            send(buttons[index], 0x0202, 0, 0xffffffff)
            send(buttons[index], 0x0200, 0, center)
            verify(index)
            previous = index
        send(main, 0x0200, 0, 735 | (12 << 16))  # Gap between Settings and Minimize.
        verify()
    print("PASS synchronized hover, press and leave on all three title buttons", flush=True)


def finish(desktop, proc, main):
    click(main, 205)
    prompt = wait(lambda: desktop.find(proc, 6))
    click(prompt, 6)
    wait(lambda: wait_process(proc.handle, 0) == 0)


class Version(socketserver.StreamRequestHandler):
    def handle(self):
        def send_frame(op, data=b""):
            self.wfile.write(struct.pack("<HH", len(data), op) + data)
        def read_frame():
            head = self.rfile.read(4)
            if not head:
                return None, b""
            length, op = struct.unpack("<HH", head)
            return op, self.rfile.read(length)
        send_frame(1536)
        while True:
            op, body = read_frame()
            if op is None:
                return
            if op == 1538:
                assert len(body) == 12
                self.server.queries.append(struct.unpack_from("<I", body)[0])
                url = self.server.base.encode()
                send_frame(1539, struct.pack("<IQB", self.server.version, STAMP, len(url)) + url + bytes(8))
            else:
                assert op == 1540 and len(body) == 8
                self.server.errors.append(struct.unpack("<II", body))
                send_frame(1541)


class Patches(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        path = self.path.removeprefix("/patch/")
        self.server.requests.append(path)
        if path == self.server.block:
            self.server.entered.set()
            self.server.release.wait(8)
        data = self.server.files.get(path)
        if callable(data):
            data = data()
        if data is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        if path == self.server.block:
            self.server.delivered.set()


def rotated_xml(path):
    data = path.read_bytes()
    return ET.fromstring(bytes(((data[i-1] << 1) | (v >> 7)) & 255 for i, v in enumerate(data)))


def encode_xml(root):
    data = ET.tostring(root, encoding="utf-8")
    return bytes(((v << 7) | (data[(i+1) % len(data)] >> 1)) & 255 for i, v in enumerate(data))


def exercise_check_bars(exe, game, desk):
    large = "C_large.dat"
    with (game / large).open("wb") as file:
        file.truncate(256 << 20)
    files = {"A_small.dat": b"short", "E_empty.dat": b""}
    flags = {name: 7 for name in (*files, large)}
    for name, data in files.items():
        (game / name).write_bytes(data)
    for prefix in "BDF":
        for i in range(112):
            name = f"{prefix}_{i:03d}.dat"
            files[name] = b""
            flags[name] = 3 if i % 2 == 0 else 0
            if flags[name]:
                (game / name).touch()
    ordered = sorted([*files, large])
    large_total_width = 463 * ordered.index(large) // len(ordered)
    with (game / large).open("rb") as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        (game / "UserFileList.dat").write_bytes(catalog({**files, large: data}, flags=flags))

    get_dc = api(U, "GetDC", H, H)
    release_dc = api(U, "ReleaseDC", C.c_int, H, H)
    pixel = api(C.WinDLL("gdi32"), "GetPixel", W.DWORD, H, C.c_int, C.c_int)
    with desk.run(exe) as proc:
        main = wait(lambda: desk.find(proc, 201))
        dc = get_dc(main)
        def width(y):
            lo, hi = 0, 463
            while lo < hi:
                mid = (lo + hi) // 2
                if pixel(dc, 14 + mid, y) == 0xf0ac6f:
                    lo = mid + 1
                else:
                    hi = mid
            return lo
        positions = set()
        metadata_totals = {prefix: set() for prefix in "BDF"}
        file_label = child(main, 207)
        try:
            assert (width(485), width(517)) == (463, 463)
            click(main, 201)
            def observed_finish():
                name = caption(file_label)
                current, total = width(485), width(517)
                if caption(file_label) != name:
                    return False
                if name.endswith(large) and total == large_total_width and 0 < current < 463:
                    positions.add(current)
                for prefix in metadata_totals:
                    if f" {prefix}_" in name and current == (0 if prefix == "F" else 463):
                        metadata_totals[prefix].add(total)
                return (metadata_totals["F"] and enabled(child(main, 201)) and
                        not name.endswith(".dat") and (current, total) == (463, 463))
            wait(observed_finish, 45)
            assert len(positions) >= 2 and all(len(values) >= 2 for values in metadata_totals.values())
            assert caption(child(main, 202)) == "Start"
        finally:
            release_dc(main, dc)
        finish(desk, proc, main)
    print("PASS visible Check bars for MD5, metadata-only, skipped and empty files", flush=True)


def exercise_check(exe, game, desk, http, versions):
    names = ("a_check.bin", "Z_check.bin", "b_check.bin", "c_check.bin", "d_check.bin", "exists.bin", "off.bin",
             "HShield/HSUpdate.env", "HShield/HSUpdate.exe", "HShield/vendor.bin")
    files = {name: b"good" for name in names}
    flags = {"off.bin": 0, "exists.bin": 1, "c_check.bin": 8, "d_check.bin": 12, "HShield/HSUpdate.env": 1}
    expected_ns = 1_600_000_000_000_000_000
    ticks = expected_ns // 100 + 116444736000000000
    manifest = catalog(files, flags=flags, modified={"d_check.bin": ticks, "HShield/HSUpdate.exe": ticks})
    filelist = game / "UserFileList.dat"
    filelist.write_bytes(manifest)
    (game / "HShield").mkdir()
    for name in ("a_check.bin", "b_check.bin", "c_check.bin", "d_check.bin", "exists.bin", "HShield/HSUpdate.env", "HShield/HSUpdate.exe"):
        (game / name).write_bytes({"a_check.bin": b"x", "b_check.bin": b"oops", "exists.bin": b"x",
                                   "HShield/HSUpdate.env": b"oops"}.get(name, b"good"))
    untouched_time = (game / "HShield/HSUpdate.exe").stat().st_mtime_ns
    def url(name):
        return DATE + "/" + name + ".zip"
    for name, data in files.items():
        http.files[url(name)] = archive(Path(name).name, data)
    failures = [("Z_check.bin", 1), ("a_check.bin", 2), ("b_check.bin", 4), ("c_check.bin", 8), ("HShield/HSUpdate.env", 4)]
    with (game / "a_check.bin").open("rb") as locked:
        package = http.files[url("a_check.bin")]
        def unlock_on_retry():
            if http.requests.count(url("a_check.bin")) == 2:
                locked.close()
            return package
        http.files[url("a_check.bin")] = unlock_on_retry
        with desk.run(exe) as proc:
            main = wait(lambda: desk.find(proc, 201))
            queries = len(versions.queries)
            click(main, 201)
            wait(lambda: locked.closed and (game / "a_check.bin").read_bytes() == b"good")
            wait(lambda: enabled(child(main, 201)) and caption(child(main, 202)) == "Start")
            assert len(versions.queries) == queries and filelist.read_bytes() == manifest
            assert http.requests == [url(name) for name, _ in failures] + [url("a_check.bin")], http.requests
            assert all((game / name).read_bytes() == b"good" for name, _ in failures)
            assert (game / "d_check.bin").stat().st_mtime_ns == expected_ns
            assert (game / "HShield/HSUpdate.exe").stat().st_mtime_ns == untouched_time
            assert (game / "exists.bin").read_bytes() == b"x"
            assert not (game / "off.bin").exists() and not (game / "HShield/vendor.bin").exists()
            log = (game / "PBLauncher.log").read_text()
            for index, (name, reason) in enumerate(failures, 1):
                key = str(Path(name)) if "/" in name else ".\\" + name
                assert f"[{index}/5] {key} / error : {reason}" in log
            finish(desk, proc, main)
    http.requests.clear()
    print("PASS Check masks, file order, dates, vendor-owned files, locked-file retry and original catalog", flush=True)
    filelist.write_bytes(catalog({"net1.bin": b"ok", "net2.bin": b"ok"}))
    first = url("net1.bin")
    http.files[first] = lambda: archive("net1.bin", b"ok") if http.requests.count(first) == 3 else None
    with desk.run(exe) as proc:
        main = wait(lambda: desk.find(proc, 201))
        click(main, 201)
        wait(lambda: versions.errors)
        assert versions.errors == [(16, 0x8200000C)]
        assert http.requests == [first] * 3 + [url("net2.bin")] * 3, http.requests
        assert not (game / "net1.bin").exists()
        prompt = wait(lambda: desk.find(proc, 2) or desk.find(proc, 1))
        click(prompt, 2 if child(prompt, 2) else 1)
        wait(lambda: wait_process(proc.handle, 0) == 0)
    http.requests.clear()
    versions.errors.clear()
    print("PASS failed HTTP repair resumes at the failed entry with the original per-batch retry count", flush=True)


def exercise_check_without_catalog(exe, game, desk, http, versions):
    filelist = game / "UserFileList.dat"
    log = game / "PBLauncher.log"
    name = "session_check.bin"
    target = game / name
    payload = b"good"
    manifest = catalog({name: payload}, flags={name: 7})
    filelist.write_bytes(manifest)
    target.write_bytes(payload)
    url = DATE + "/" + name + ".zip"
    http.files[url] = archive(name, payload)
    with desk.run(exe) as proc:
        main = wait(lambda: desk.find(proc, 201))
        wait(lambda: enabled(child(main, 201)) and caption(child(main, 202)) == "Start")
        queries = len(versions.queries)

        def check():
            offset = log.stat().st_size
            click(main, 201)
            wait(lambda: b"-> [UPDATER_STATE_PATCH_END]" in log.read_bytes()[offset:])
            wait(lambda: enabled(child(main, 201)) and caption(child(main, 202)) == "Start")
            assert len(versions.queries) == queries
            return log.read_bytes()[offset:]

        filelist.unlink()
        assert b"failed user file list open" in check()
        assert not filelist.exists() and not http.requests

        damaged = manifest[:len(manifest) // 2]
        filelist.write_bytes(damaged)
        target.write_bytes(b"oops")
        assert b"failed user file list open" in check()
        assert target.read_bytes() == payload and http.requests == [url]
        assert filelist.read_bytes() == damaged
        http.requests.clear()

        filelist.write_bytes(manifest)
        open_file = api(K, "CreateFileW", H, W.LPCWSTR, W.DWORD, W.DWORD, H, W.DWORD, W.DWORD, H)
        handle = open_file(str(filelist), 0x80000000, 0, None, 3, 0, None)
        if handle == H(-1).value:
            raise C.WinError(C.get_last_error())
        try:
            assert b"failed user file list open" in check()
            assert not http.requests
        finally:
            close(handle)

        updated = b"updated"
        filelist.write_bytes(catalog({name: updated}, flags={name: 7}))
        http.files[url] = archive(name, updated)
        assert b"failed user file list open" not in check()
        assert target.read_bytes() == updated and http.requests == [url]
        http.requests.clear()
        finish(desk, proc, main)

    filelist.write_bytes(catalog({}))
    with desk.run(exe) as proc:
        main = wait(lambda: desk.find(proc, 201))
        wait(lambda: enabled(child(main, 201)) and caption(child(main, 202)) == "Start")
        filelist.unlink()
        click(main, 201)
        wait(lambda: not enabled(child(main, 201)) and caption(child(main, 202)) == "Update")
        assert not http.requests
        finish(desk, proc, main)
    print("PASS Check handles a missing, truncated, locked or restored catalogue after startup", flush=True)


def exercise(base, versions, http, desk, next_client):
    game = base / "game"
    game.mkdir()
    # Run under a different filename to detect any dependency on the installed program's name.
    exe = game / "Cliente de teste.exe"
    release = ROOT / "build/client/Release"
    shutil.copy2(release / "PBLauncher.exe", exe)
    for name in ("ResourceCheck.exe", "Launcher.svl", "config.zpt", "Messages.xml", "UI.xml"):
        shutil.copy2(release / name, game / name)
    messages_path = game / "Messages.xml"
    messages = ET.parse(messages_path)
    assert messages.findtext("BUTTON_PAUSE") == "Pausar"
    overrides = {
        "INIT_WAIT_MSG": "Abrindo o teste…", "QUESTION_EXIT": "Encerrar o teste?",
        "STATE_SIMPLE_PATCH_END": "Pronto para o teste.", "MAIN_FILE": "Arquivo de teste",
        "MAIN_TOTAL": "Total de teste", "BUTTON_PAUSE": "Parar",
        "STATE_SIMPLE_PATCHDOWN": "Transferindo o arquivo de teste.", "STATE_PATCH_PAUSED": "Pausa de teste.",
    }
    for key, value in overrides.items():
        messages.find(key).text = value
    messages.write(messages_path, encoding="utf-8", xml_declaration=True)
    source = base / "Probe.cs"
    source.write_text('using System.IO; class Probe { static void Main(string[] a) { File.WriteAllText("started", string.Join(" ",a)); } }')
    compiler = Path(os.environ["WINDIR"]) / "Microsoft.NET/Framework/v4.0.30319/csc.exe"
    subprocess.run([str(compiler), "/nologo", "/target:winexe", "/out:" + str(game / "Probe.exe"),
                    str(source)], check=True)
    files = {"A.bin": b"new A", "B.bin": b"new B", "C.bin": b"stable"}
    name = DATE + "_FileList.dat"
    http.files[name + ".zip"] = archive(name, catalog(files))
    for key, data in files.items():
        http.files[DATE + "/" + key + ".zip"] = archive(key, data)
    cfg_xml = rotated_xml(game / "config.zpt")
    cfg_xml.find("server_address").text = "127.0.0.1"
    cfg_xml.find("server_port").text = str(versions.server_address[1])
    cfg_xml.find("patcherror_retry_count").text = "3"
    cfg_xml.find("executable_file").text = "Probe.exe"
    cfg_xml.find("web_url").text = "about:blank"
    notice = cfg_xml.find("notice_ui")
    if notice is None:
        notice = ET.SubElement(cfg_xml, "notice_ui")
    notice.text = "hide"
    (game / "config.zpt").write_bytes(encode_xml(cfg_xml))
    log = game / "PBLauncher.log"
    log.write_bytes(b"x" * (2 << 20))
    spec = importlib.util.spec_from_file_location("publish_patch", ROOT / "server/PublishPatch.py")
    publisher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(publisher)
    for fmt in range(1, 5):
        (game / "UserFileList.dat").write_bytes(catalog({**files, "x,y;z": b""}, fmt=fmt))
        version, parsed = publisher.read_catalog(game / "UserFileList.dat")
        assert version == DATE and [p[0].name for p in parsed] == [*files, "x,y;z"]
        with desk.run(exe, cwd=base) as proc:
            if fmt == 1:
                splash = wait(lambda: desk.find(proc, 1017))
                assert caption(child(splash, 1017)) == overrides["INIT_WAIT_MSG"]
            def ready():
                box = desk.find(proc, 1) or desk.find(proc, 2)
                if box:
                    raise AssertionError((caption(child(box, 65535)), log.read_bytes()[-280:], http.requests))
                return desk.find(proc, 201)
            try:
                view = wait(ready)
            except AssertionError as err:
                raise AssertionError((fmt, log.read_bytes()[-280:], versions.queries, http.requests)) from err
            wait(lambda: caption(child(view, 202)) == "Start")
            assert not visible(child(view, 221))
            if fmt == 1:
                exercise_title_buttons(view)
                for ident, key in [(206, "STATE_SIMPLE_PATCH_END"), (207, "MAIN_FILE"), (220, "MAIN_TOTAL")]:
                    assert caption(child(view, ident)) == overrides[key]
                click(view, 205)
                prompt = wait(lambda: desk.find(proc, 7))
                assert caption(child(prompt, 65535)) == overrides["QUESTION_EXIT"]
                click(prompt, 7)
                wait(lambda: enabled(view))
                assert visible(view)
            finish(desk, proc, view)
    print("PASS catalog formats 1–4 and renamed executable", flush=True)

    filelist = game / "UserFileList.dat"
    missing = game / "A.bin"
    modified = game / "B.bin"
    modified.write_bytes(b"bad B")  # Same length as the catalog entry.
    for content, button in [(catalog(files), "Start"), (catalog({}), "Start"),
                            (catalog(files, stamp=STAMP-1), "Update"), (None, "Update"),
                            (b"damaged catalog", "Update")]:
        if content is None:
            filelist.unlink()
        else:
            filelist.write_bytes(content)
        with desk.run(exe) as proc:
            main = wait(lambda: desk.find(proc, 201))
            primary = child(main, 202)
            wait(lambda: enabled(primary) and caption(primary) == button)
            assert bool(enabled(child(main, 201))) == (button == "Start")
            assert not missing.exists() and modified.read_bytes() == b"bad B"
            assert not http.requests
            finish(desk, proc, main)
    filelist.write_bytes(catalog(files))
    print("PASS startup offers Start or Update from the local catalog version without repairing game files", flush=True)

    assert log.stat().st_size < 65536 and "PBLauncher End" in log.read_text()
    print("PASS hidden news and two-MiB log rotation", flush=True)
    ui = game / "UI.xml"
    good = ui.read_bytes()
    ui_revision = rotated_xml(game / "Launcher.svl").find("file[@name='UI.xml']").get("version")
    ui_stem = "UI_" + ui_revision
    ui_zip = "_LauncherPatchFiles/" + ui_stem + ".zip"
    ui.write_bytes(b"<ui")
    http.files[ui_zip] = archive(ui_stem + ".xml", good)
    with desk.run(exe) as proc:
        view = wait(lambda: desk.find(proc, 201))
        assert ui.read_bytes() == good and http.requests == [ui_zip]
        finish(desk, proc, view)
    http.requests.clear()
    print("PASS corrupt UI repaired by downloading only its component", flush=True)
    setfile = game / "Launcher.svl"
    original_set = setfile.read_bytes()
    setfile.write_bytes(b"invalid")
    label = f"Launcher_{GAME_VERSION}"
    http.files["_LauncherPatchFiles/" + label + ".zip"] = archive(label + ".svl", original_set)
    with desk.run(exe) as proc:
        view = wait(lambda: desk.find(proc, 201))
        assert setfile.read_bytes() == original_set and http.requests == ["_LauncherPatchFiles/" + label + ".zip"]
        finish(desk, proc, view)
    http.requests.clear()
    print("PASS corrupt launcher set repaired without reloading installed components", flush=True)
    exercise_check_bars(exe, game, desk)
    exercise_check(exe, game, desk, http, versions)
    exercise_check_without_catalog(exe, game, desk, http, versions)

    with desk.run(exe, "/check_string") as proc:
        for index in range(47):
            def current():
                h = desk.find(proc, 6)
                return h if h and f"({index}/47)" in caption(child(h, 65535)) else None
            box = wait(current)
            click(box, 6)
        wait(lambda: wait_process(proc.handle, 0) == 0)
    print("PASS all 47 diagnostic messages in binary table order", flush=True)

    obsolete = "Old.bin"
    local = {"B.bin": b"old B", "C.bin": files["C.bin"], obsolete: b"old"}
    (game / "UserFileList.dat").write_bytes(catalog(local, stamp=STAMP-1))
    for key, data in local.items():
        (game / key).write_bytes(data)
    http.block = DATE + "/A.bin.zip"
    with desk.run(exe) as proc:
        main = wait(lambda: desk.find(proc, 201))
        assert not enabled(child(main, 201))
        click(main, 202)
        wait(http.entered.is_set)
        assert (game / "B.bin").read_bytes() == local["B.bin"]
        wait(lambda: caption(child(main, 202)) == overrides["BUTTON_PAUSE"])
        assert caption(child(main, 206)) == overrides["STATE_SIMPLE_PATCHDOWN"]
        assert caption(child(main, 207)) == overrides["MAIN_FILE"] + " A.bin"
        click(main, 202)
        wait(lambda: caption(child(main, 202)) == "Update")
        assert caption(child(main, 206)) == overrides["STATE_PATCH_PAUSED"]
        http.release.set()
        wait(http.delivered.is_set)
        assert (game / "B.bin").read_bytes() == local["B.bin"]
        click(main, 202)
        wait(lambda: caption(child(main, 202)) == "Start")
        assert (game / "UserFileList.dat").read_bytes() == catalog(files)
        assert not (game / obsolete).exists()
        assert http.requests == [name+".zip", DATE+"/B.bin.zip", DATE+"/A.bin.zip"], http.requests
        print("PASS XML captions on splash, exit, progress and pause; pause/resume preserves staged files", flush=True)
        print("PASS delta, staged download/extract/apply, deletion, atomic catalogue", flush=True)

        http.files.pop(name + ".zip")
        http.requests.clear()
        (game / "B.bin").write_bytes(b"bad B")
        k = len(versions.queries)
        click(main, 201)
        wait(lambda: (game / "B.bin").read_bytes() == files["B.bin"])
        wait(lambda: enabled(child(main, 201)))
        assert http.requests == [DATE+"/B.bin.zip"] and len(versions.queries) == k
        print("PASS Check automatically repairs only the failed entry using local catalogue", flush=True)
        admin = C.WinDLL("shell32").IsUserAnAdmin()
        if not admin:
            (game / "Probe.exe").rename(game / "Probe.held")
        click(main, 202)
        if admin:
            wait(lambda: (game / "started").exists())
            assert (game / "started").read_text(encoding="utf-8-sig") == "1" and len(versions.queries) == k + 1
            print("PASS Start issues a fresh handshake and launches the probe with argument 1", flush=True)
        else:
            # The private desktop must never trigger an interactive UAC prompt on the user's desktop.
            wait(lambda: versions.errors)
            assert versions.errors[-1] in ((16, 0x82000017), (16, 0x82000030)), versions.errors
            assert len(versions.queries) == k + 2 and not (game / "started").exists()
            box = wait(lambda: desk.find(proc, 2) or desk.find(proc, 1))
            click(box, 2 if child(box, 2) else 1)
            print("PASS Start rechecks version, reports missing game or administrator and closes", flush=True)
        wait(lambda: wait_process(proc.handle, 0) == 0)
        versions.errors.clear()
    (game / "B.bin").write_bytes(b"bad B")
    http.files[DATE+"/B.bin.zip"] = archive("B.bin", b"bad B")
    http.requests.clear()
    with desk.run(exe) as proc:
        main = wait(lambda: desk.find(proc, 201))
        click(main, 201)
        wait(lambda: versions.errors)
        assert versions.errors == [(16, 0x82000027)], versions.errors
        assert http.requests == [DATE+"/B.bin.zip"] * 4
        box = wait(lambda: desk.find(proc, 2) or desk.find(proc, 1))
        click(box, 2 if child(box, 2) else 1)
        wait(lambda: wait_process(proc.handle, 0) == 0)
    print("PASS MD5 retries, exact TCP error state/code, no invalid replacement", flush=True)

    assert (game / "B.bin").read_bytes() == b"bad B"
    (game / "UserFileList.dat").write_bytes(catalog(files, stamp=STAMP-1))
    http.requests.clear()
    with desk.run(exe) as proc:
        main = wait(lambda: desk.find(proc, 201))
        versions.version += 1
        click(main, 202)
        wait(lambda: len(versions.errors) == 2)
        assert versions.errors[-1] == (16, 0x82000011) and not http.requests
        box = wait(lambda: desk.find(proc, 2) or desk.find(proc, 1))
        click(box, 2 if child(box, 2) else 1)
        wait(lambda: wait_process(proc.handle, 0) == 0)
    print("PASS changed launcher set between Connect and Update detected before HTTP", flush=True)
    if next_client:
        manifest = rotated_xml(game / "Launcher.svl")
        manifest.set("version", str(versions.version))
        manifest.find("file[@name='Launcher']").set("version", str(versions.version))
        stem = "_LauncherPatchFiles/"
        for prefix, ext, data in [("Launcher", ".svl", encode_xml(manifest)),
                                  ("PBLauncher", ".exe", next_client.read_bytes())]:
            versioned = f"{prefix}_{versions.version}"
            http.files[stem + versioned + ".zip"] = archive(versioned + ext, data)
        for installed in (exe, game / "PBLauncher.exe"):
            shutil.copy2(release / "Launcher.svl", game / "Launcher.svl")
            shutil.copy2(release / "PBLauncher.exe", installed)
            files_before = {f.name for f in game.glob("*.exe")}
            with desk.run(installed, cwd=base) as proc:
                wait(lambda: wait_process(proc.handle, 0) == 0, 20)
                view = wait(lambda: desk.find(None, 201), 20)
                assert installed.read_bytes() == next_client.read_bytes()
                assert {f.name for f in game.glob("*.exe")} == files_before
                pid = W.DWORD()
                api(U, "GetWindowThreadProcessId", W.DWORD, H, C.POINTER(W.DWORD))(view, C.byref(pid))
                relaunched = Process(pid=pid.value)
                relaunched.handle = api(K, "OpenProcess", H, W.DWORD, W.BOOL, W.DWORD)(0x100000, False, pid)
                try:
                    finish(desk, relaunched, view)
                finally:
                    close(relaunched.handle)
            installed.unlink()
        print("PASS ResourceCheck replaces and restarts both PBLauncher.exe and a renamed client", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--next-client", type=Path, help="Native client built with version 20171215")
    options = parser.parse_args()
    desk = Desktop()
    try:
        with ThreadingHTTPServer(("127.0.0.1", 0), Patches) as http, socketserver.ThreadingTCPServer(
                ("127.0.0.1", 0), Version) as versions, tempfile.TemporaryDirectory(prefix="PB-parity-") as tmp:
            versions.queries, versions.errors, versions.version = [], [], GAME_VERSION
            versions.base = f"http://127.0.0.1:{http.server_address[1]}/patch"
            http.files, http.requests, http.block = {}, [], None
            http.entered, http.release, http.delivered = (threading.Event() for _ in range(3))
            for server in (versions, http):
                threading.Thread(target=server.serve_forever, daemon=True).start()
            try:
                exercise(Path(tmp), versions, http, desk, options.next_client)
            finally:
                http.release.set()
                for server in (versions, http):
                    server.shutdown()
    finally:
        desk.switch(desk.previous)
        api(U, "CloseDesktop", W.BOOL, H)(desk.handle)
