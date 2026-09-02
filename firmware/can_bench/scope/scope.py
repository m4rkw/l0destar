"""Rigol DHO814 SCPI helper: scope.py idn | shot out.png | cmd ':...' | q ':...?'"""
import socket, sys, time
HOST = "10.1.4.77"; PORT = 5555

class Scope:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
    def query(self, c, timeout=10):
        self.s.settimeout(timeout); self.cmd(c)
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = self.s.recv(65536)
            if not chunk: break
            buf += chunk
        return buf.decode(errors="replace").strip()
    def block(self, c, timeout=30):
        self.s.settimeout(timeout); self.cmd(c)
        hdr = b""
        while len(hdr) < 2: hdr += self.s.recv(2 - len(hdr))
        assert hdr[0:1] == b"#", hdr
        nd = int(hdr[1:2]); 
        while len(hdr) < 2 + nd: hdr += self.s.recv(2 + nd - len(hdr))
        n = int(hdr[2:2 + nd]); data = b""
        while len(data) < n:
            chunk = self.s.recv(min(65536, n - len(data)))
            if not chunk: break
            data += chunk
        try: self.s.recv(2)
        except Exception: pass
        return data
    def shot(self, path):
        data = self.block(":DISPlay:DATA? PNG") if False else self.block(":DISP:DATA?")
        open(path, "wb").write(data); return len(data)

if __name__ == "__main__":
    sc = Scope(); a = sys.argv[1:]
    if not a or a[0] == "idn": print(sc.query("*IDN?"))
    elif a[0] == "shot": print(sc.shot(a[1]), "bytes")
    elif a[0] == "q": print(sc.query(a[1]))
    elif a[0] == "cmd": sc.cmd(a[1]); print("ok")
