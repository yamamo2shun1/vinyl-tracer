# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Shunichi Yamamoto
"""Write the latest release (version and date) into the promo site pages.

Usage: python3 tools/site_fill_release.py <tag> <published_at (ISO 8601, UTC)>

Run by the Pages workflow before uploading site/. The pages keep a fallback
text inside <span class="release" data-release>…</span>; this replaces it.
The date is shown in Japan time (releases are published from Japan).
"""
import re
import sys
from datetime import datetime, timedelta, timezone

JST = timezone(timedelta(hours=9))
PAGES = {
    "site/index.html": "Latest: {tag} · {date}",
    "site/ja/index.html": "最新版 {tag}（{date}）",
}
SPAN = re.compile(r'(<span class="release" data-release>)[^<]*(</span>)')


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tag, published = sys.argv[1], sys.argv[2]
    date = datetime.fromisoformat(published.replace("Z", "+00:00")).astimezone(JST).strftime("%Y-%m-%d")
    for path, fmt in PAGES.items():
        with open(path, encoding="utf-8", newline="") as f:
            html = f.read()
        text = fmt.format(tag=tag, date=date)
        html, n = SPAN.subn(lambda m: m.group(1) + text + m.group(2), html)
        if n == 0:
            print(f"{path}: no release placeholder found", file=sys.stderr)
            return 1
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(html)
        print(f"{path}: {tag} {date} ({n} places)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
