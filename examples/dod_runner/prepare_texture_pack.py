"""Stage the public DoD exclusion manifest into an explicitly selected pack.
Dry run by default; refuses to replace an existing different manifest or modify textures.
"""
import argparse
from pathlib import Path
import sys


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack',required=True,type=Path)
    parser.add_argument('--confirmed-game-revision',required=True,choices=['SLPM_662.75'])
    parser.add_argument('--confirmed-pack',required=True,choices=['DoD_HD_CN-reference'])
    parser.add_argument('--apply',action='store_true')
    args=parser.parse_args(argv)
    if not args.pack.is_dir():parser.error('--pack must be an existing pack root')
    source=Path(__file__).with_name('texture_exclusions.txt')
    dest=args.pack/'texture_exclusions.txt'
    expected=source.read_bytes()
    if dest.exists():
        if dest.read_bytes()==expected:
            print('Reference exclusions already present');return 0
        parser.error('existing manifest differs; merge it manually instead of overwriting')
    if not args.apply:
        print(f'DRY RUN: would create {dest}; textures stay untouched');return 0
    try:
        with dest.open('xb') as output:output.write(expected)
    except FileExistsError:
        parser.error('manifest appeared during preparation; refusing to overwrite')
    print(f'Created {dest}; textures stay untouched');return 0

if __name__=='__main__':sys.exit(main())
