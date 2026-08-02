#!/usr/bin/env python3
"""本地 mock 服务器：/sse 返回 chunked SSE 流，/json 返回 JSON，/mcp/* 为 MCP SSE 传输端点。
供 C 测试真实网络往返。"""
import json
import os
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
toolcall_hits = 0
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
        if os.environ.get('MCP_DEBUG'):
            sys.stderr.write('[mcp-debug] POST body: ' + body + '\n')
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

    def _mcp_stream(self, use_202):
        """Streamable HTTP：POST JSON-RPC。直答(200+JSON) 或 202+SSE 事件流。
        session 校验：首次发 'sess-1'，后续必须匹配，否则 404。"""
        length = int(self.headers.get('Content-Length', 0) or 0)
        body = self.rfile.read(length).decode('utf-8', 'replace') if length else ''
        if os.environ.get('MCP_DEBUG'):
            sys.stderr.write('[mcp-debug] stream POST body: ' + repr(body) + '\n')
        sess = self.headers.get('mcp-session-id')
        expected = 'sess-1'
        if sess is None:
            new_session = expected
        elif sess == expected:
            new_session = None
        else:
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        # 解析请求并构造响应
        resp = None
        try:
            req = json.loads(body) if body else {}
            method = req.get('method')
            rid = req.get('id')
            if method == 'tools/list':
                resp = {'jsonrpc': '2.0', 'id': rid, 'result': {'tools': [
                    {'name': 'echo', 'description': 'Echo tool',
                     'inputSchema': {'type': 'object'}}]}}
            elif method == 'tools/call':
                args = (req.get('params') or {}).get('arguments') or {}
                text = args.get('text', '')
                resp = {'jsonrpc': '2.0', 'id': rid,
                        'result': {'content': [{'type': 'text', 'text': 'echo: ' + text}]}}
            else:
                resp = {'jsonrpc': '2.0', 'id': rid,
                        'error': {'code': -32601, 'message': 'not found'}}
        except Exception:
            pass
        payload = json.dumps(resp).encode('utf-8')
        if use_202:
            sse = ('event: message\ndata: ' + json.dumps(resp) + '\n\n').encode('utf-8')
            self.send_response(202)
            if new_session:
                self.send_header('mcp-session-id', new_session)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Content-Length', str(len(sse)))
            self.end_headers()
            self.wfile.write(sse)
        else:
            self.send_response(200)
            if new_session:
                self.send_header('mcp-session-id', new_session)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    def do_POST(self):
        if self.path == '/mcp/messages':
            self._mcp_message()
            return
        if self.path == '/mcp/stream':
            self._mcp_stream(use_202=False)
            return
        if self.path == '/mcp/stream202':
            self._mcp_stream(use_202=True)
            return
        if self.path == '/oauth/register':
            self._oauth_register()
            return
        if self.path == '/oauth/token':
            self._oauth_token()
            return
        length = int(self.headers.get('Content-Length', 0) or 0)
        if length:
            self._body = self.rfile.read(length)
        else:
            self._body = b''
        self.do_GET()

    def _oauth_meta(self):
        """OAuth 受保护资源元数据 (RFC 9728)"""
        base = 'http://127.0.0.1:%d/oauth' % self.server.server_address[1]
        meta = {'resource': base + '/resource',
                'authorization_servers': [base + '/as']}
        body = json.dumps(meta).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _oauth_as_wellknown(self):
        """授权服务器元数据 (RFC 8414)"""
        base = 'http://127.0.0.1:%d/oauth' % self.server.server_address[1]
        md = {'issuer': base + '/as',
              'authorization_endpoint': base + '/authorize',
              'token_endpoint': base + '/token',
              'registration_endpoint': base + '/register'}
        body = json.dumps(md).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _oauth_register(self):
        length = int(self.headers.get('Content-Length', 0) or 0)
        body = self.rfile.read(length).decode('utf-8', 'replace') if length else ''
        req = json.loads(body) if body else {}
        resp = {'client_id': 'client-1'}
        if req.get('scope'):
            resp['scope'] = req['scope']
        payload = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _oauth_token(self):
        length = int(self.headers.get('Content-Length', 0) or 0)
        body = self.rfile.read(length).decode('utf-8', 'replace') if length else ''
        params = dict(kv.split('=', 1) for kv in body.split('&') if '=' in kv)
        grant = params.get('grant_type')
        if grant == 'authorization_code':
            resp = {'access_token': 'at-1', 'token_type': 'Bearer',
                    'expires_in': 3600, 'refresh_token': 'rt-1',
                    'scope': params.get('scope') or ''}
        elif grant == 'refresh_token':
            resp = {'access_token': 'at-2', 'token_type': 'Bearer',
                    'expires_in': 3600}
        else:
            self.send_response(400)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        payload = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path == '/oauth/resource':
            self.send_response(401)
            self.send_header('WWW-Authenticate',
                             'Bearer resource_metadata="http://127.0.0.1:%d/oauth/meta"'
                             % self.server.server_address[1])
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if self.path == '/oauth/meta':
            self._oauth_meta()
            return
        if self.path == '/oauth/as/.well-known/oauth-authorization-server':
            self._oauth_as_wellknown()
            return
        if self.path == '/oauth/authorize':
            # 模拟授权页：302 到 redirect_uri?code=code-1&state=...
            from urllib.parse import urlparse, parse_qs, urlencode
            q = parse_qs(urlparse(self.path).query)
            redirect = q.get('redirect_uri', [''])[0]
            state = q.get('state', [''])[0]
            loc = redirect + '?code=code-1&state=' + state
            self.send_response(302)
            self.send_header('Location', loc)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if self.path == '/redirect':
            # 重定向测试钩子：302 → /openai（相对路径，跟随后应拿到 chat SSE 流）
            self.send_response(302)
            self.send_header('Location', '/openai')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if self.path == '/mcp/sse':
            self._sse_stream('/mcp/messages')  # 相对 endpoint（主路径）
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
        elif self.path == '/chat/completions':
            # 请求体含 tools → 工具循环回放（首轮 tool_calls，后续最终文本）；
            # 否则与 /openai 相同文本回放（OCR 等无工具场景）。
            # body 由 do_POST 预先读取并缓存（避免二次读 rfile 死锁）。
            global toolcall_hits
            _body = getattr(self, '_body', b'')
            if b'"tools"' not in _body:
                self.path = '/openai'
                self.do_GET()
            else:
                toolcall_hits += 1
                if toolcall_hits == 1:
                    evs = [
                        'data: {"id":"1","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"get_time_info","arguments":""}}]},"finish_reason":null}]}\n\n',
                        'data: {"id":"1","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{}"}}]},"finish_reason":null}]}\n\n',
                        'data: {"id":"1","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]}\n\n',
                        'data: [DONE]\n\n',
                    ]
                else:
                    evs = [
                        'data: {"id":"2","choices":[{"index":0,"delta":{"content":"Final "},"finish_reason":null}]}\n\n',
                        'data: {"id":"2","choices":[{"index":0,"delta":{"content":"answer"},"finish_reason":"stop"}]}\n\n',
                        'data: [DONE]\n\n',
                    ]
                self.send_response(200)
                self.send_header('Content-Type', 'text/event-stream')
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
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
        elif self.path == '/toolcall':
            toolcall_hits += 1
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            if toolcall_hits == 1:
                # 第一轮：请求工具调用 get_time_info
                evs = [
                    'data: {"id":"1","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"get_time_info","arguments":""}}]},"finish_reason":null}]}\n\n',
                    'data: {"id":"1","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{}"}}]},"finish_reason":null}]}\n\n',
                    'data: {"id":"1","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]}\n\n',
                    'data: [DONE]\n\n',
                ]
            else:
                # 后续轮：最终文本
                evs = [
                    'data: {"id":"2","choices":[{"index":0,"delta":{"content":"Final "},"finish_reason":null}]}\n\n',
                    'data: {"id":"2","choices":[{"index":0,"delta":{"content":"answer"},"finish_reason":"stop"}]}\n\n',
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
        elif self.path == '/slow':
            # 慢速流：每 200ms 一个 tick，共 5 个（约 1s）。
            # http_sse_split_events 断言 tick0-4；chat_cancel 需流足够慢。
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            for i in range(5):
                e = 'data: tick%d\n\n' % i
                try:
                    b = e.encode()
                    self.wfile.write(('%x\r\n' % len(b)).encode() + b + b'\r\n')
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, ValueError):
                    break
                time.sleep(0.2)
            try:
                self.wfile.write(b'0\r\n\r\n')
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ValueError):
                pass
        elif self.path == '/parallel':
            # OpenAI 并行工具调用回放（index 0 + index 1）
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            evs = [
                'data: {"id":"1","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function","function":{"name":"get_time_info","arguments":""}}]},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{"tool_calls":[{"index":1,"id":"call_b","type":"function","function":{"name":"memory_tool","arguments":"{\\"action\\":"}}]},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{}"}}]},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{"tool_calls":[{"index":1,"function":{"arguments":"\\"create\\",\\"content\\":\\"hi\\"}"}}]},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]}\n\n',
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
        elif self.path == '/think':
            # 流式 think_tag: 标签跨块切分（测试状态机前缀匹配）
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            evs = [
                'data: {"id":"1","choices":[{"index":0,"delta":{"content":"<th"},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{"content":"ink>deep rea"},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{"content":"soning</th"},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{"content":"ink>Final "},"finish_reason":null}]}\n\n',
                'data: {"id":"1","choices":[{"index":0,"delta":{"content":"answer"},"finish_reason":"stop"}]}\n\n',
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
                'data: {"id":"1","choices":[],"usage":{"prompt_tokens":12,"completion_tokens":5,"total_tokens":17}}\n\n',
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
                'event: message_delta\ndata: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":7}}\n\n',
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
        elif self.path == '/claude_tool':
            # Claude 工具调用回放（tool_use + input_json_delta 流式累积）
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            evs = [
                'event: message_start\ndata: {"type":"message_start","message":{"id":"m1"}}\n\n',
                'event: content_block_start\ndata: {"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"toolu_1","name":"get_time_info"}}\n\n',
                'event: content_block_delta\ndata: {"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\\"time\\":"}}\n\n',
                'event: content_block_delta\ndata: {"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"\\"now\\"}"}}\n\n',
                'event: content_block_stop\ndata: {"type":"content_block_stop","index":0}\n\n',
                'event: message_delta\ndata: {"type":"message_delta","delta":{"stop_reason":"tool_use"}}\n\n',
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
        elif self.path == '/google_tool':
            # Google functionCall 回放（完整对象）
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            evs = [
                'data: {"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"get_time_info","args":{"time":"now"}}}]}}]}\n\n',
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
    request_queue_size = 128  # 并发测试：默认 5 的 backlog 会拒掉并发连接


if __name__ == '__main__':
    ReuseServer(('127.0.0.1', PORT), H).serve_forever()
