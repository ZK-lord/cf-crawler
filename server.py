#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
本地开发服务器 — 支持从浏览器添加用户并自动抓取完整数据
"""

import os, sys, json, subprocess, threading, urllib.parse
from http.server import HTTPServer, SimpleHTTPRequestHandler

BASE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(BASE, 'output')
SAMPLE_TXT = os.path.join(BASE, 'sample_users.txt')
EXE = os.path.join(BASE, 'bin', 'cf_crawler.exe')

PORT = 8088

class AddUserHandler(SimpleHTTPRequestHandler):
    """支持 /api/add-user 的 HTTP 服务器"""

    def do_POST(self):
        if self.path == '/api/add-user':
            length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(length).decode('utf-8')
            params = urllib.parse.parse_qs(body)
            handles = params.get('handle', [])
            if not handles:
                self.send_json(400, {'ok': False, 'error': '缺少 handle 参数'})
                return

            handle = handles[0].strip()
            if not handle:
                self.send_json(400, {'ok': False, 'error': 'handle 为空'})
                return

            # 读取现有用户
            existing = set()
            if os.path.exists(SAMPLE_TXT):
                with open(SAMPLE_TXT, 'r', encoding='utf-8') as f:
                    for line in f:
                        h = line.strip()
                        if h:
                            existing.add(h.lower())

            if handle.lower() in existing:
                self.send_json(200, {'ok': True, 'message': f'{handle} 已在列表中，跳过'})
                return

            # 追加到 sample_users.txt
            with open(SAMPLE_TXT, 'a', encoding='utf-8') as f:
                f.write(handle + '\n')

            # 后台运行 C 程序
            def run_crawler():
                env = os.environ.copy()
                env['PATH'] = r'C:\msys64\ucrt64\bin;' + env.get('PATH', '')
                subprocess.run([EXE], cwd=BASE, env=env, capture_output=True)

            thread = threading.Thread(target=run_crawler, daemon=True)
            thread.start()
            thread.join(timeout=120)

            self.send_json(200, {'ok': True, 'message': f'{handle} 已添加并完成数据抓取'})
        else:
            self.send_error(404)

    def send_json(self, status, data):
        body = json.dumps(data, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def translate_path(self, path):
        # 优先从 output/ 目录提供文件
        if path == '/' or path == '':
            path = '/index.html'
        output_path = os.path.join(OUTPUT, path.lstrip('/'))
        if os.path.exists(output_path):
            return output_path
        return super().translate_path(path)


if __name__ == '__main__':
    os.chdir(BASE)
    server = HTTPServer(('127.0.0.1', PORT), AddUserHandler)
    print(f'启动成功！请访问 http://127.0.0.1:{PORT}')
    print(f'在页面输入 CF 用户名后点击"添加"，服务器会自动抓取完整数据')
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\n已停止')
        server.server_close()
