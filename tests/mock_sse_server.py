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
                try:
                    b = s.encode('utf-8')
                    self.wfile.write(('%x\r\n' % len(b)).encode() + b + b'\r\n')
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, ValueError):
                    pass

            chunk('data: Hello\n\n')
            time.sleep(0.05)
            chunk('data: world\n\n')
            time.sleep(0.05)
            chunk('event: done\ndata: \n\n')
            time.sleep(0.05)
            try:
                self.wfile.write(b'0\r\n\r\n')  # chunked 结束
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ValueError):
                pass
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
                try:
                    self.wfile.write(('data: tick%d\n\n' % i).encode())
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, ValueError):
                    break
                time.sleep(0.05)
            try:
                self.wfile.close()
            except Exception:
                pass
        elif self.path == '/fail500':
            self.send_response(500)
            self.send_header('Content-Length', '0')
            self.end_headers()
        elif self.path == '/sse_bad':
            # 畸形 SSE：超长行（> 8KB）触发解析错误
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            bad = 'data: ' + 'x' * 20000 + '\n\n'
            b = bad.encode()
            try:
                self.wfile.write(('%x\r\n' % len(b)).encode() + b + b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ValueError):
                pass
        elif self.path == '/openai':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            evs = [
                'data: {"id":"1","choices":[{"index":0,"delta":{"content":"Hello "},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{"content":"world"},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{"reasoning_content":"think"},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\n\n',
                'data: [DONE]\n\n',
            ]
            for e in evs:
                try:
                    b = e.encode()
                    self.wfile.write(('%x\r\n' % len(b)).encode() + b + b'\r\n')
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, ValueError):
                    break
            try:
                self.wfile.write(b'0\r\n\r\n')
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ValueError):
                pass
        elif self.path == '/claude':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            evs = [
                'event: message_start\ndata: {"type":"message_start","message":{"id":"m1"}}\n\n',
                'event: content_block_delta\ndata: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hi "}}\n\n',
                'event: content_block_delta\ndata: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"there"}}\n\n',
                'event: content_block_delta\ndata: {"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"hmm"}}\n\n',
                'event: message_delta\ndata: {"type":"message_delta","delta":{"stop_reason":"end_turn"}}\n\n',
            ]
            for e in evs:
                try:
                    b = e.encode()
                    self.wfile.write(('%x\r\n' % len(b)).encode() + b + b'\r\n')
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, ValueError):
                    break
            try:
                self.wfile.write(b'0\r\n\r\n')
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ValueError):
                pass
        elif self.path == '/google':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            evs = [
                'data: {"candidates":[{"content":{"role":"model","parts":[{"text":"Google "}]}}]}\n\n',
                'data: {"candidates":[{"content":{"role":"model","parts":[{"text":"answer"}]}}]}\n\n',
            ]
            for e in evs:
                try:
                    b = e.encode()
                    self.wfile.write(('%x\r\n' % len(b)).encode() + b + b'\r\n')
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, ValueError):
                    break
            try:
                self.wfile.write(b'0\r\n\r\n')
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ValueError):
                pass
        else:
            self.send_response(404)
            self.end_headers()


class ReuseServer(HTTPServer):
    allow_reuse_address = True


if __name__ == '__main__':
    ReuseServer(('127.0.0.1', PORT), H).serve_forever()
