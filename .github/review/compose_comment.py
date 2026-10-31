#!/usr/bin/env python3
"""Build the PR comment of the review workflow (.github/workflows/pr-review.yml).

Reads the report the review wrote and Claude Code's JSON result, and prints
one Markdown comment: a marker line (how the workflow finds the comment to
update on a re-run), the report, cut to fit GitHub's comment limit, and a
footer naming the model, the tokens used and where the full run lives.

Standard library only; the PR's content never runs, it is only quoted.
"""

import argparse
import json
import pathlib
import sys

MARKER = "<!-- ce-code-review:crowbot -->"
# GitHub refuses a comment body longer than 65536 characters. Leave room for
# the header, the footer and the truncation note.
GITHUB_LIMIT = 65536
FOOTER_ROOM = 2000


def read_text(path):
    if not path:
        return ""
    p = pathlib.Path(path)
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace").strip()


def read_result(path):
    """Claude Code's --output-format json result, or {} when it is missing."""
    text = read_text(path)
    if not text:
        return {}
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        return {}
    return value if isinstance(value, dict) else {}


def truncate(report, budget, artifact_url):
    if len(report) <= budget:
        return report
    cut = report[:budget]
    # Cut at a line end so a table row or a word is not split.
    newline = cut.rfind("\n")
    if newline > budget // 2:
        cut = cut[:newline]
    # An odd number of fences means the cut landed inside a code block.
    if cut.count("```") % 2 == 1:
        cut += "\n```"
    where = f"[the run's artifact]({artifact_url})" if artifact_url else "the run's artifact"
    return (
        cut
        + "\n\n> **Report truncated** to fit GitHub's comment limit"
        + f" ({len(report)} characters). The full report is `report.md` in {where}."
    )


def usage_line(result):
    usage = result.get("modelUsage")
    if not isinstance(usage, dict) or not usage:
        return ""
    parts = []
    for model, u in usage.items():
        if not isinstance(u, dict):
            continue
        tokens_in = (
            u.get("inputTokens", 0)
            + u.get("cacheReadInputTokens", 0)
            + u.get("cacheCreationInputTokens", 0)
        )
        parts.append(f"`{model}` {tokens_in:,} in / {u.get('outputTokens', 0):,} out")
    if not parts:
        return ""
    line = "Tokens: " + ", ".join(parts)
    cost = result.get("total_cost_usd")
    if isinstance(cost, (int, float)):
        # Claude Code prices the tokens at Anthropic list prices; crowbot bills
        # by its own, so this is an estimate only.
        line += f" (about ${cost:.2f} at Anthropic list prices; crowbot bills its own rates)"
    return line


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", help="report.md the review wrote")
    ap.add_argument("--result", help="Claude Code's JSON result")
    ap.add_argument("--status", required=True, help="outcome of the review step")
    ap.add_argument("--run-url", required=True)
    ap.add_argument("--artifact-url", default="")
    ap.add_argument("--model", default="")
    ap.add_argument("--head-sha", default="")
    args = ap.parse_args()

    result = read_result(args.result)
    report = read_text(args.report)
    if not report and isinstance(result.get("result"), str):
        report = result["result"].strip()

    lines = [MARKER, "## Automated code review (ce-code-review)", ""]
    if args.status != "success" or not report:
        lines.append(
            f"The review did not finish (step outcome: `{args.status}`)."
            f" See [the workflow run]({args.run_url}) for its log."
        )
        if report:
            lines += ["", "Partial output:", ""]
    footer = ["", "---"]
    meta = []
    if args.head_sha:
        meta.append(f"Reviewed head `{args.head_sha[:12]}`")
    if args.model:
        meta.append(f"model `{args.model}` via crowbot.sh")
    meta.append(f"[workflow run]({args.run_url})")
    if args.artifact_url:
        meta.append(f"[review artifacts]({args.artifact_url})")
    footer.append("<sub>" + " | ".join(meta) + "</sub>")
    tokens = usage_line(result)
    if tokens:
        footer.append(f"<sub>{tokens}</sub>")
    footer.append(
        "<sub>To re-run, add the `review` label again. This comment is"
        " updated in place.</sub>"
    )

    head = "\n".join(lines)
    tail = "\n".join(footer)
    budget = GITHUB_LIMIT - len(head) - len(tail) - FOOTER_ROOM
    body = head + ("\n" + truncate(report, budget, args.artifact_url) if report else "") + "\n" + tail
    assert len(body) <= GITHUB_LIMIT, len(body)
    sys.stdout.write(body + "\n")


if __name__ == "__main__":
    main()
