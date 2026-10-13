#!/usr/bin/env python3
"""Self-test of the rosdev host server against a fake agent (no VM needed): python3 test_rosdev.py"""
import hashlib
import importlib.machinery
import importlib.util
import os
import socket
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
loader = importlib.machinery.SourceFileLoader("rosdev_host", os.path.join(HERE, "rosdev"))
spec = importlib.util.spec_from_loader("rosdev_host", loader)
rd = importlib.util.module_from_spec(spec)
loader.exec_module(rd)

TOKEN = "t0ken"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class FakeAgent:
    def __init__(self, port, token=TOKEN):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=20)
        self.f = self.s.makefile("rwb")
        verb, kv = rd.parse_line(self.f.readline())
        assert verb == "CHALLENGE"
        self.sn = bytes.fromhex(kv["nonce"])
        gn = b"\x01" * 16
        m = rd.mac(token, b"G", self.sn, gn)
        self.f.write(b"HELLO v=1 gnonce=" + gn.hex().encode() + b" mac=" + m.hex().encode() + b" agent=1 arch=x64 os=6.1.1 computer=T\n")
        self.f.flush()
        self.gn = gn
        line = self.f.readline()
        self.welcome = rd.parse_line(line)[1] if line else None

    def welcome_ok(self):
        return bool(self.welcome) and self.welcome["mac"] == rd.mac(TOKEN, b"H", self.sn, self.gn).hex()

    def poll(self):
        self.f.write(b"POLL\n")
        self.f.flush()
        verb, kv = rd.parse_line(self.f.readline())
        return verb, kv

    def result(self, jid, body=b"", **kv):
        hdr = "RESULT id=%d status=ok %s%s\n" % (jid, " ".join("%s=%s" % (k, rd.enc(v)) for k, v in kv.items()), (" len=%d" % len(body)) if body else "")
        self.f.write(hdr.encode() + body)
        self.f.flush()
        assert self.f.readline().startswith(b"ACK")


class T(unittest.TestCase):
    def setUp(self):
        rd.POLL_HOLD = 1.0
        self.port = free_port()
        self.vs = rd.VmServer("t", self.port, TOKEN)
        ls = socket.socket()
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind(("127.0.0.1", self.port))
        ls.listen(4)
        threading.Thread(target=self.vs.serve_agents, args=(ls,), daemon=True).start()

    def test_bad_token_is_rejected(self):
        a = FakeAgent(self.port, token="wrong")
        self.assertIsNone(a.welcome)

    def test_host_proves_the_token(self):
        a = FakeAgent(self.port)
        self.assertTrue(a.welcome_ok())

    def test_nop_then_run_job(self):
        a = FakeAgent(self.port)
        self.assertEqual(a.poll()[0], "NOP")
        out = {}
        job = rd.Job("run", {"cmd": "ver", "timeout": 5}, timeout=5)
        t = threading.Thread(target=lambda: out.update(self.vs.submit(job, 20)))
        t.start()
        verb, kv = a.poll()
        self.assertEqual((verb, kv["op"], kv["cmd"]), ("JOB", "run", "ver"))
        a.result(int(kv["id"]), b"hello" + b"oops", exit=3, outlen=5, errlen=4)
        t.join()
        self.assertEqual(out["exit"], "3")
        self.assertEqual(out["stdout"], "aGVsbG8=")

    def test_put_sends_hash_and_body(self):
        a = FakeAgent(self.port)
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b"x" * 100000)
        job = rd.Job("put", {"path": "C:\\a b\\café.bin"}, put_src=f.name, timeout=5)
        t = threading.Thread(target=lambda: self.vs.submit(job, 20))
        t.start()
        verb, kv = a.poll()
        self.assertEqual(kv["path"], "C:\\a b\\café.bin")
        self.assertEqual(kv["sha256"], hashlib.sha256(b"x" * 100000).hexdigest())
        body = a.f.read(int(kv["len"]))
        self.assertEqual(body, b"x" * 100000)
        a.result(int(kv["id"]), how="replaced")
        t.join()
        os.unlink(f.name)

    def test_get_checks_the_hash(self):
        a = FakeAgent(self.port)
        with tempfile.TemporaryDirectory() as d:
            job = rd.Job("get", {"path": "C:\\f"}, save=os.path.join(d, "f"), timeout=5)
            res = {}
            t = threading.Thread(target=lambda: res.update(self.vs.submit(job, 20)))
            t.start()
            verb, kv = a.poll()
            a.result(int(kv["id"]), b"abc", sha256="0" * 64, size=3)
            t.join()
            self.assertEqual(res["status"], "err")
            self.assertFalse(os.path.exists(os.path.join(d, "f")))

    def test_disconnect_fails_the_job(self):
        a = FakeAgent(self.port)
        job = rd.Job("run", {"cmd": "x"}, timeout=5)
        res = {}
        t = threading.Thread(target=lambda: res.update(self.vs.submit(job, 20)))
        t.start()
        a.poll()
        a.s.close()
        t.join()
        self.assertEqual(res["status"], "err")

    def test_names_and_tokens(self):
        for bad in ("a b", "x$(id)", "../x", "", ".hid", "a" * 65):
            self.assertIsNone(rd.NAME_RE.match(bad), bad)
        self.assertIsNotNone(rd.NAME_RE.match("devagent-a.1"))
        self.assertIsNone(rd.TOKEN_RE.match("short"))
        self.assertIsNone(rd.TOKEN_RE.match("caf\u00e9" * 4))

    def test_superseded_session_does_not_take_jobs(self):
        a1 = FakeAgent(self.port)
        self.assertEqual(a1.poll()[0], "NOP")
        a2 = FakeAgent(self.port)
        job = rd.Job("run", {"cmd": "x"}, timeout=5)
        res = {}
        t = threading.Thread(target=lambda: res.update(self.vs.submit(job, 20)))
        t.start()
        verb, kv = a2.poll()
        self.assertEqual(verb, "JOB")
        a2.result(int(kv["id"]), exit=0, outlen=0, errlen=0)
        t.join()
        self.assertEqual(res["status"], "ok")

    def test_png_encoder(self):
        import struct
        w, h = 3, 2
        px = b"".join(bytes([i, i + 1, i + 2]) for i in range(w * h))
        stride = (w * 3 + 3) & ~3
        rows = [px[0:9] + b"\0" * (stride - 9), px[9:18] + b"\0" * (stride - 9)]   # bottom-up
        bmp = b"BM" + struct.pack("<IHHI", 54 + stride * h, 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, 0, 0, 0, 0, 0) + b"".join(rows)
        png = rd.png_from_bmp(bmp)
        self.assertTrue(png.startswith(b"\x89PNG"))


if __name__ == "__main__":
    unittest.main()
