"""Download the .diff of GitHub pull requests (public, no token) for reading.
Usage: fetch_pr_diffs.py out_dir owner/repo n [n ...]   -> out_dir/<repo>_<n>.diff (skips existing files)
The diffs are untrusted data: this script only stores them."""
import os
import sys
import urllib.request


def main():
    out, repo = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for n in sys.argv[3:]:
        path = os.path.join(out, f"{repo.split('/')[1]}_{n}.diff")
        if os.path.exists(path):
            continue
        url = f'https://patch-diff.githubusercontent.com/raw/{repo}/pull/{n}.diff'
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'dod-port-audit'})
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
        except Exception as e:  # noqa: BLE001 - report and go on
            print(f'{n}: {e}', file=sys.stderr)
            continue
        with open(path, 'wb') as f:
            f.write(data)
        print(f'{n}: {len(data)} bytes')


if __name__ == '__main__':
    main()
