from __future__ import annotations

import argparse
import http.server
import socketserver
import webbrowser
from pathlib import Path


DEFAULT_PORT = 8000


def main() -> int:
    parser = argparse.ArgumentParser(description="在本地通过 HTTP 启动 Interactive BOM。")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="本地 HTTP 端口，默认 8000")
    parser.add_argument(
        "--file",
        default="InteractiveBOM_PCB130_2026-3-12.html",
        help="要打开的 Interactive BOM 文件名",
    )
    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="只启动 HTTP 服务，不自动打开浏览器",
    )
    args = parser.parse_args()

    base_dir = Path(__file__).resolve().parent
    target_file = base_dir / args.file
    if not target_file.exists():
        print(f"未找到文件: {target_file}")
        return 1

    handler = http.server.SimpleHTTPRequestHandler
    url = f"http://127.0.0.1:{args.port}/{target_file.name}"

    with socketserver.TCPServer(("127.0.0.1", args.port), handler) as httpd:
        print(f"本地 HTTP 服务已启动: {url}")
        print("按 Ctrl+C 停止服务。")
        if not args.no_browser:
            webbrowser.open(url)
        try:
            import os

            os.chdir(base_dir)
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n已停止本地 HTTP 服务。")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
