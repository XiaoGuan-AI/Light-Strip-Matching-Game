from __future__ import annotations

import argparse
from pathlib import Path


SCRIPT_NAME = "interactive_bom_bridge.js"
SCRIPT_TAG = f'<script src="./{SCRIPT_NAME}"></script>'


def inject_bridge(html_path: Path) -> bool:
    content = html_path.read_text(encoding="utf-8")

    if SCRIPT_TAG in content:
        return False

    lower_content = content.lower()
    body_close_index = lower_content.rfind("</body>")
    html_close_index = lower_content.rfind("</html>")

    if body_close_index != -1:
        updated = content[:body_close_index] + SCRIPT_TAG + content[body_close_index:]
    elif html_close_index != -1:
        updated = content[:html_close_index] + SCRIPT_TAG + content[html_close_index:]
    else:
        updated = content + "\n" + SCRIPT_TAG

    html_path.write_text(updated, encoding="utf-8")
    return True


def resolve_targets(base_dir: Path, patterns: list[str]) -> list[Path]:
    if not patterns:
        patterns = ["InteractiveBOM*.html"]

    matches: list[Path] = []
    for pattern in patterns:
        matches.extend(sorted(base_dir.glob(pattern)))

    unique_matches = []
    seen = set()
    for path in matches:
        normalized = path.resolve()
        if normalized in seen or not path.is_file():
            continue
        seen.add(normalized)
        unique_matches.append(path)
    return unique_matches


def main() -> int:
    parser = argparse.ArgumentParser(
        description="为嘉立创导出的 Interactive BOM HTML 注入双击亮灯桥接脚本。"
    )
    parser.add_argument(
        "patterns",
        nargs="*",
        help="要处理的 HTML 文件名或 glob，默认处理当前目录下的 InteractiveBOM*.html",
    )
    args = parser.parse_args()

    base_dir = Path(__file__).resolve().parent
    targets = resolve_targets(base_dir, args.patterns)
    if not targets:
        print("没有找到可处理的 Interactive BOM HTML 文件。")
        return 1

    changed_count = 0
    for target in targets:
        changed = inject_bridge(target)
        status = "已注入" if changed else "已存在"
        print(f"{status}: {target.name}")
        changed_count += int(changed)

    print(f"处理完成，共更新 {changed_count} 个文件。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
