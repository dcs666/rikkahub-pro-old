#!/usr/bin/env python3
"""本地 mock 服务器：/sse 返回 chunked SSE 流，/json 返回 JSON，/mcp/* 为 MCP SSE 传输端点。
供 C 测试真实网络往返。"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18888

# MCP SSE：活跃 SSE 流注册表（wfile），POST /mcp/messages 时向所有流广播响应
streams_lock = threading.Lock()
streams = {}  # id(self.wfile) -> wfile

# 重试中间件测试钩子：/flaky 前 2 次 500，之后成功；/flaky429 同理
flaky_hits = 0
flaky429_hits = 0


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    # ---- MCP SSE transport ----
    def _sse_stream(self, endpoint_url, heartbeat=True):
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Cache-Control', 'no-cache')
        self.end_headers()
        sid = id(self.wfile)
        with streams_lock:
            streams[sid] = self.wfile
        try:
            self.wfile.write(b'event: endpoint\ndata: ' + endpoint_url.encode('utf-8') + b'\n\n')
            self.wfile.flush()
            while True:
                time.sleep(1)
                if not heartbeat:
                    continue  # 无心跳：探测客户端读超时处理
                self.wfile.write(b': ping\n\n')  # 心跳同时探测客户端断开
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ValueError):
            pass
        finally:
            with streams_lock:
                streams.pop(sid, None)

    def _mcp_message(self):
        length = int(self.headers.get('Content-Length', 0) or 0)
        body = self.rfile.read(length).decode('utf-8', 'replace') if length else ''
        resp = {'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': 'parse error'}}
        try:
            req = json.loads(body)
            rid = req.get('id')
            method = req.get('method', '')
            if method == 'tools/list':
                resp = {'jsonrpc': '2.0', 'id': rid, 'result': {'tools': [
                    {'name': 'echo', 'description': 'Echo tool',
                     'inputSchema': {'type': 'object'}}]}}
            elif method == 'tools/call':
                args = (req.get('params') or {}).get('arguments') or {}
                text = args.get('text', '')
                if text == 'boom':
                    # 测试钩子：JSON-RPC error 响应
                    resp = {'jsonrpc': '2.0', 'id': rid,
                            'error': {'code': -32000, 'message': 'boom'}}
                else:
                    resp = {'jsonrpc': '2.0', 'id': rid,
                            'result': {'content': [{'type': 'text', 'text': 'echo: ' + text}]}}
            else:
                resp = {'jsonrpc': '2.0', 'id': rid,
                        'error': {'code': -32601, 'message': 'not found'}}
        except Exception:
            pass
        payload = ('event: message\ndata: ' + json.dumps(resp) + '\n\n').encode('utf-8')
        with streams_lock:
            for w in list(streams.values()):
                try:
                    w.write(payload)
                    w.flush()
                except (BrokenPipeError, ConnectionResetError, ValueError):
                    pass
        self.send_response(202)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def do_POST(self):
        if self.path == '/mcp/messages':
            self._mcp_message()
            return
        length = int(self.headers.get('Content-Length', 0) or 0)
        if length:
            self.rfile.read(length)
        self.do_GET()

    def do_GET(self):
        if self.path == '/mcp/sse':
            self._sse_stream('/mcp/messages')  # 相对 endpoint（主路径）
            return
        if self.path == '/mcp/sse_abs':
            # 绝对 endpoint URL（测试 parse_endpoint 绝对分支）
            port = self.server.server_address[1]
            self._sse_stream('http://127.0.0.1:%d/mcp/messages' % port)
            return
        if self.path == '/mcp/sse_nohb':
            # 无心跳流（测试客户端读超时后继续等待，不退出传输）
            self._sse_stream('/mcp/messages', heartbeat=False)
            return
        if self.path == '/mcp/sse_bad':
            # endpoint 指向不存在的路径（POST 404 → 调用立即失败）
            self._sse_stream('/mcp/nowhere')
            return
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
        elif self.path == '/chat/completions':
            # 与 /openai 相同的 SSE 回放（CLI 默认路径）
            self.path = '/openai'
            self.do_GET()
        elif self.path == '/fail500':
            body = json.dumps({'error': {'message': 'boom 500'}}).encode('utf-8')
            self.send_response(500)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == '/flaky':
            global flaky_hits
            flaky_hits += 1
            if flaky_hits <= 2:  # 前两次 500 → 触发重试，第三次成功
                body = json.dumps({'error': {'message': 'flaky 500'}}).encode('utf-8')
                self.send_response(500)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.path = '/openai'
            self.do_GET()
            return
        elif self.path == '/flaky429':
            global flaky429_hits
            flaky429_hits += 1
            if flaky429_hits <= 2:  # 429 限流 → 重试
                body = json.dumps({'error': {'message': 'rate limited'}}).encode('utf-8')
                self.send_response(429)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.path = '/openai'
            self.do_GET()
            return
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


class ReuseServer(ThreadingHTTPServer):
    allow_reuse_address = True


if __name__ == '__main__':
    ReuseServer(('127.0.0.1', PORT), H).serve_forever()
