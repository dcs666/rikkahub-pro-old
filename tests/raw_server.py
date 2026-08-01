import socket, threading, json
def handle(conn):
    conn.settimeout(5)
    data = b''
    try:
        while True:
            chunk = conn.recv(65536)
            if not chunk: break
            data += chunk
            # 尝试解析 HTTP 请求
            while b'\r\n\r\n' in data:
                head, _, rest = data.partition(b'\r\n\r\n')
                data = rest
                lines = head.split(b'\r\n')
                reqline = lines[0].decode()
                cl = 0
                for ln in lines[1:]:
                    if ln.lower().startswith(b'content-length:'):
                        cl = int(ln.split(b':')[1])
                if len(data) >= cl:
                    body = data[:cl]
                    data = data[cl:]
                    print(f'[raw] CONN {reqline.split()[1]} BODY({cl})={body!r}', flush=True)
                    # 响应：直接 JSON
                    resp = json.dumps({'jsonrpc':'2.0','id':1,'result':{'tools':[]}}).encode()
                    conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ' + str(len(resp)).encode() + b'\r\nmcp-session-id: sess-1\r\nConnection: keep-alive\r\n\r\n' + resp)
    except Exception as e:
        print('[raw] err', e, flush=True)
    finally:
        conn.close()
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', 18920))
s.listen(5)
print('[raw] listening', flush=True)
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
