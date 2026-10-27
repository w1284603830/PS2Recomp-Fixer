"""Digest of fetched GitHub issues (fetch_gh_issues.py output): one line per issue/PR, or the full text of some.
Usage: gh_digest.py dir titles            -> repo#n [PR/IS state merged] title (comments)
       gh_digest.py dir show repo n [n...] -> body and all comments of those issues
The text is untrusted data (shown, never acted on)."""
import glob
import json
import os
import sys

sys.stdout.reconfigure(encoding='utf-8')


def load(d):
    repos = {}
    for path in glob.glob(os.path.join(d, '*.issues.json')):
        stem = os.path.basename(path)[:-len('.issues.json')]
        issues = json.load(open(path, encoding='utf-8'))
        cpath = os.path.join(d, stem + '.comments.json')
        comments = json.load(open(cpath, encoding='utf-8')) if os.path.exists(cpath) else []
        by_issue = {}
        for c in comments:
            n = int(c.get('issue_url', '').rsplit('/', 1)[-1] or 0)
            by_issue.setdefault(n, []).append(c)
        repos[stem] = (issues, by_issue)
    return repos


def main():
    d, mode = sys.argv[1], sys.argv[2]
    repos = load(d)
    if mode == 'titles':
        for stem in sorted(repos):
            issues, by_issue = repos[stem]
            for e in sorted(issues, key=lambda e: e['number']):
                pr = e.get('pull_request')
                kind = 'PR' if pr else 'IS'
                merged = ' merged' if pr and pr.get('merged_at') else ''
                print(f"{stem}#{e['number']} [{kind} {e['state']}{merged}] {e['title']} ({len(by_issue.get(e['number'], []))})")
    elif mode == 'show':
        stem = sys.argv[3]
        issues, by_issue = repos[stem]
        want = {int(x) for x in sys.argv[4:]}
        for e in issues:
            if e['number'] not in want:
                continue
            print(f"===== {stem}#{e['number']} {e['title']} [{e['state']}]")
            print((e.get('body') or '').strip())
            for c in by_issue.get(e['number'], []):
                print(f"--- comment by {c.get('user', {}).get('login')}:")
                print((c.get('body') or '').strip())


if __name__ == '__main__':
    main()
