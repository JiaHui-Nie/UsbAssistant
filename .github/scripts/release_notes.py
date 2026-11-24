#!/usr/bin/env python3
"""根据两个 release tag 之间的提交生成 Release 说明。

提交信息约定（中文、单行、可能包含多段）：
    v1.1.2: fix: 修复某个问题; ci: 调整 workflow

前缀 `vX.Y.Z: ` 会被去掉，剩余内容按 `;` / `；` 拆成多段，每段按
`<type>: <描述>` 归类。无法识别的段归入「其他」。
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

VERSION_TAG_RE = re.compile(r"^v\d+(?:\.\d+)*$")
VERSION_PREFIX_RE = re.compile(r"^\s*v\d+(?:\.\d+)*\s*[:：]\s*", re.IGNORECASE)
SEGMENT_TYPE_RE = re.compile(
    r"^(feat|fix|perf|refactor|docs|doc|test|build|ci|chore)"
    r"(?:\([^)]*\))?!?\s*[:：]\s*(.+)$",
    re.IGNORECASE,
)

TYPE_ALIASES = {"doc": "docs"}

CATEGORY_ORDER = [
    ("feat", "新功能（feat）"),
    ("fix", "修复（fix）"),
    ("perf", "性能（perf）"),
    ("refactor", "重构（refactor）"),
    ("docs", "文档（docs）"),
    ("test", "测试（test）"),
    ("build", "构建（build）"),
    ("ci", "持续集成（ci）"),
    ("chore", "杂项（chore）"),
    ("other", "其他"),
]


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", "-c", "core.quotepath=false", *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            "git %s failed: %s"
            % (" ".join(args), result.stderr.decode("utf-8", "replace").strip())
        )
    return result.stdout.decode("utf-8", "replace")


def commit_of(ref: str) -> str:
    return git("rev-parse", ref + "^{commit}").strip()


def previous_release_tag(tag: str, commit: str) -> str | None:
    # 优先用 git describe：取当前 tag 提交的父提交之前最近的一个 tag。
    try:
        described = git("describe", "--tags", "--abbrev=0", commit + "^").strip()
        if VERSION_TAG_RE.match(described) and described != tag:
            return described
    except RuntimeError:
        pass

    # 回退：所有已合并、版本号形态的 tag 中版本最高者（排除当前 tag）。
    merged = git("tag", "--merged", commit, "--list", "v*", "--sort=-v:refname")
    for candidate in merged.splitlines():
        candidate = candidate.strip()
        if (
            candidate
            and candidate != tag
            and VERSION_TAG_RE.match(candidate)
        ):
            return candidate
    return None


def split_segments(subject: str) -> list[tuple[str, str]]:
    """把一条提交标题拆成 (类别, 描述) 列表。

    第一段没有类型前缀时整条提交归入「其他」；后续段缺少类型时
    继承前一段的类型，描述文本按提交内去重。
    """
    text = VERSION_PREFIX_RE.sub("", subject.strip())
    if not text:
        return []

    segments = [part.strip() for part in re.split(r"[;；]", text)]
    entries: list[tuple[str, str]] = []
    seen: set[tuple[str, str]] = set()
    current: str | None = None
    for segment in segments:
        if not segment:
            continue
        match = SEGMENT_TYPE_RE.match(segment)
        if match:
            category = match.group(1).lower()
            current = TYPE_ALIASES.get(category, category)
            description = match.group(2).strip()
        elif current is not None:
            description = segment
        else:
            # 第一段就没有类型前缀：整条提交无法归类。
            return [("other", text)]

        item = (current, description)
        if description and item not in seen:
            seen.add(item)
            entries.append(item)
    return entries


def collect(tag: str, from_tag: str | None, commit: str) -> list[tuple[str, str]]:
    if from_tag:
        base = commit_of(from_tag)
        rev_range = base + ".." + commit
        print("range: %s..%s" % (from_tag, tag), file=sys.stderr)
    else:
        rev_range = commit
        print("range: first commit..%s" % tag, file=sys.stderr)

    raw = git("log", "--reverse", "--pretty=format:%s%x1e", rev_range)
    entries: list[tuple[str, str]] = []
    for subject in raw.split("\x1e"):
        subject = subject.strip()
        if subject:
            entries.extend(split_segments(subject))
    return entries


def render(tag: str, from_tag: str | None, entries: list[tuple[str, str]]) -> str:
    lines = ["## 变更内容", ""]
    if from_tag:
        lines.append("区间：`%s` → `%s`" % (from_tag, tag))
    else:
        lines.append("区间：首个提交 → `%s`" % tag)
    lines.append("")

    if not entries:
        lines.append("本版本区间内没有可识别的提交。")
        return "\n".join(lines).rstrip() + "\n"

    grouped: dict[str, list[str]] = {}
    for category, description in entries:
        grouped.setdefault(category, []).append(description)

    for category, title in CATEGORY_ORDER:
        items = grouped.get(category)
        if not items:
            continue
        lines.append("### " + title)
        lines.append("")
        for item in items:
            lines.append("- " + item)
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    # Windows 下 stdio 默认跟随本地代码页（CI runner 是 cp1252），中文提交
    # 信息在 print 时会抛 UnicodeEncodeError；统一改成 UTF-8 输出。
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--tag",
        default=os.environ.get("GITHUB_REF_NAME", ""),
        help="当前 release tag（默认取 GITHUB_REF_NAME）",
    )
    parser.add_argument("--from", dest="from_tag", help="强制指定上一个 tag，便于本地验证")
    parser.add_argument("--output", default="release_notes.md", help="输出文件路径")
    args = parser.parse_args()

    tag = args.tag.strip()
    if not tag:
        print("缺少 --tag 或 GITHUB_REF_NAME", file=sys.stderr)
        return 1

    try:
        commit = commit_of(tag)
    except RuntimeError as error:
        print("无法解析 tag %s：%s" % (tag, error), file=sys.stderr)
        return 1

    from_tag = args.from_tag.strip() if args.from_tag else None
    if from_tag is None:
        from_tag = previous_release_tag(tag, commit)

    try:
        entries = collect(tag, from_tag, commit)
    except RuntimeError as error:
        print("读取提交失败：%s" % error, file=sys.stderr)
        return 1

    body = render(tag, from_tag, entries)
    with open(args.output, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(body)
    print(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
