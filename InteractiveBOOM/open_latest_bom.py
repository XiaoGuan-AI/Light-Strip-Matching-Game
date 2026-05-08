from __future__ import annotations

import argparse
import http.server
import os
import socketserver
import urllib.parse
import webbrowser
from pathlib import Path

from inject_interactive_bom_bridge import inject_bridge


DEFAULT_PORT = 8000


def get_latest_bom(base_dir: Path) -> Path | None:
    candidates = sorted(
        base_dir.glob("InteractiveBOM*.html"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def build_target_url(file_name: str, port: int, base_url: str) -> str:
    url = f"http://127.0.0.1:{port}/{file_name}"
    normalized_base_url = base_url.strip()
    if not normalized_base_url:
        return url

    query = urllib.parse.urlencode({"baseUrl": normalized_base_url})
    return f"{url}?{query}"


class ReusableTCPServer(socketserver.TCPServer):
    allow_reuse_address = True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="一键处理并打开最新导出的 Interactive BOM。"
    )
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="本地 HTTP 端口，默认 8000")
    parser.add_argument(
        "--base-url",
        default="",
        help="分拣箱后端地址，例如 http://192.168.3.238:32323",
    )
    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="只启动 HTTP 服务，不自动打开浏览器",
    )
    args = parser.parse_args()

    base_dir = Path(__file__).resolve().parent
    target_file = get_latest_bom(base_dir)
    if target_file is None:
        print("当前目录没有找到 InteractiveBOM*.html 文件。")
        return 1

    changed = inject_bridge(target_file)
    print(f"使用文件: {target_file.name}")
    print("桥接脚本状态: " + ("已注入" if changed else "已存在"))

    os.chdir(base_dir)
    url = build_target_url(target_file.name, args.port, args.base_url)
    handler = http.server.SimpleHTTPRequestHandler

    with ReusableTCPServer(("127.0.0.1", args.port), handler) as httpd:
        print(f"本地 HTTP 服务已启动: {url}")
        if args.base_url.strip():
            print(f"分拣箱后端地址: {args.base_url.strip()}")
        print("按 Ctrl+C 停止服务。")
        if not args.no_browser:
            webbrowser.open(url)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n已停止本地 HTTP 服务。")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
