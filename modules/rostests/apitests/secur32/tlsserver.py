#!/usr/bin/env python3
# tlsserver.py PORT MIN MAX CERT KEY [CURVE] : HTTPS server limited to TLS versions MIN..MAX (1.2 or 1.3), serves ~200 KB
import socket, ssl, sys, threading
port, lo, hi, cert, key = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
curve = sys.argv[6] if len(sys.argv) > 6 else None
V = {'1.2': ssl.TLSVersion.TLSv1_2, '1.3': ssl.TLSVersion.TLSv1_3}
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version, ctx.maximum_version = V[lo], V[hi]
ctx.load_cert_chain(cert, key)
if curve:
    ctx.set_ecdh_curve(curve)
body = (b'0123456789abcdef' * 4096) * 3
resp = b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n' % len(body) + body
def handle(c):
    try:
        t = ctx.wrap_socket(c, server_side=True)
        t.recv(4096)
        t.sendall(resp)
        t.shutdown(socket.SHUT_RDWR)
        t.close()
    except Exception as e:
        print('conn error', e, flush=True)
        c.close()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(50)
print('listening', port, lo, hi, flush=True)
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
