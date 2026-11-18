#!/usr/bin/env python3
"""Serve this folder over HTTP with caching disabled, so a browser in the VM sees every edit.

  ~/bvm/dev/webtest/serve.py [port]        (default 8000, listens on all interfaces)

In the VM (QEMU user network) the host is 10.0.2.2:   http://10.0.2.2:8000/
The test page reloads itself when index.html changes.

Guest -> host file upload (saved to ~/bvm/scratchpad/guest-out/, max 64 MB), from a Windows command prompt:
  curl.exe -T C:\\bvm\\update-diag.txt http://10.0.2.2:8000/upload/update-diag.txt
"""
import http.server
import os
import sys

UPLOAD_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'guest-out')
MAX_UPLOAD = 64 * 1024 * 1024


class NoCache(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, max-age=0')
        super().end_headers()

    def do_PUT(self):
        name = os.path.basename(self.path.split('?')[0])
        length = int(self.headers.get('Content-Length', 0))
        if not self.path.startswith('/upload/') or not name or length > MAX_UPLOAD:
            self.send_error(400, 'PUT /upload/<name>, at most 64 MB')
            return
        os.makedirs(UPLOAD_DIR, exist_ok=True)
        with open(os.path.join(UPLOAD_DIR, name), 'wb') as f:
            f.write(self.rfile.read(length))
        self.send_response(201)
        self.send_header('Content-Length', '0')
        self.end_headers()
        print('uploaded %s (%d bytes)' % (name, length), flush=True)


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    srv = http.server.ThreadingHTTPServer(('0.0.0.0', port), NoCache)
    print('serving %s on http://0.0.0.0:%d/  (VM: http://10.0.2.2:%d/)' % (os.getcwd(), port, port), flush=True)
    srv.serve_forever()
