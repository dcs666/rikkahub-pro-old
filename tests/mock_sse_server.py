#!/usr/bin/env python3
"""本地 mock 服务器：/sse 返回 chunked SSE 流，/json 返回 JSON。供 C 测试真实网络往返。"""
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import sys
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18888

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0) or 0)
        if length:
            self.rfile.read(length)
        self.do_GET()

    def do_GET(self):
        if self.path == '/sse':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()

            def chunk(s):
                b = s.encode('utf-8')
                self.wfile.write(('%x\r\n' % len(b)).encode() + b + b'\r\n')
                self.wfile.flush()

            chunk('data: Hello\n\n')
            time.sleep(0.05)
            chunk('data: world\n\n')
            time.sleep(0.05)
            chunk('event: done\ndata: \n\n')
            time.sleep(0.05)
            self.wfile.write(b'0\r\n\r\n')  # chunked 结束
            self.wfile.flush()
        elif self.path == '/json':
            body = b'{"ok":true,"value":42}'
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == '/slow':
            # 无长度流式，持续输出
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            for i in range(5):
                self.wfile.write(('data: tick%d\n\n' % i).encode())
                self.wfile.flush()
                time.sleep(0.05)
            self.wfile.close()
        else:
            self.send_response(404)
            self.end_headers()


if __name__ == '__main__':
    HTTPServer(('127.0.0.1', PORT), H).serve_forever()
