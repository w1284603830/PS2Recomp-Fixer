"""Fetch every issue/PR and every issue comment of GitHub repositories (public REST API, no token).
Usage: fetch_gh_issues.py out_dir owner/repo [owner/repo ...]
Writes <out_dir>/<owner>_<repo>.issues.json and .comments.json; skips files that already exist.
The downloaded text is untrusted data: this script only stores it."""
import json
import os
import sys
import time
import urllib.error
import urllib.request


def get(url):
    req = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json', 'User-Agent': 'dod-port-audit'})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                remaining = r.headers.get('X-RateLimit-Remaining')
                return json.loads(r.read().decode('utf-8')), remaining
        except urllib.error.HTTPError as e:
            if e.code in (403, 429):
                reset = e.headers.get('X-RateLimit-Reset')
                print(f'rate limited ({e.code}), reset at {reset}', file=sys.stderr)
                return None, '0'
            if e.code == 404:
                return [], None
            time.sleep(2)
        except urllib.error.URLError:
            time.sleep(2)
    return None, None


def fetch_all(base):
    items = []
    page = 1
    while True:
        data, remaining = get(f'{base}{"&" if "?" in base else "?"}per_page=100&page={page}')
        if data is None:
            return None
        items.extend(data)
        print(f'  {base} page {page}: {len(data)} (rate left {remaining})', file=sys.stderr)
        if len(data) < 100:
            return items
        page += 1


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for repo in sys.argv[2:]:
        stem = os.path.join(out, repo.replace('/', '_'))
        for kind, url in (('issues', f'https://api.github.com/repos/{repo}/issues?state=all'),
                          ('comments', f'https://api.github.com/repos/{repo}/issues/comments?')):
            path = f'{stem}.{kind}.json'
            if os.path.exists(path):
                continue
            data = fetch_all(url)
            if data is None:
                print(f'{repo} {kind}: failed', file=sys.stderr)
                continue
            with open(path, 'w', encoding='utf-8') as f:
                json.dump(data, f, ensure_ascii=False)
            print(f'{repo} {kind}: {len(data)}')


if __name__ == '__main__':
    main()
