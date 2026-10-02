#!/usr/bin/env python3
"""Refresh, or with --check verify, server/tests/data/client_dialect.json against msime-cloud.

The file is msime-cloud's internal/skins/testdata/client_dialect.json: the cases the cross-platform client's skin loader accepts and rejects, which the community library validates packages with. The server test candidate_skin_catalog_loads_every_package_the_client_accepts loads every accepted case, so a rule the client adds reaches this repository as a failing check rather than as community skins that vanish from the Windows list.
"""
import argparse
import sys
import urllib.request
from pathlib import Path

SOURCE = 'https://raw.githubusercontent.com/metasequoiaime/msime-cloud/{ref}/internal/skins/testdata/client_dialect.json'
TARGET = Path(__file__).resolve().parents[1] / 'server/tests/data/client_dialect.json'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--ref', default='main', help='msime-cloud branch, tag or commit to read (default: main)')
    parser.add_argument('--check', action='store_true', help='fail when the copy differs instead of rewriting it')
    args = parser.parse_args()
    with urllib.request.urlopen(SOURCE.format(ref=args.ref), timeout=30) as response:
        upstream = response.read()
    if args.check:
        if TARGET.read_bytes() != upstream:
            print(f'{TARGET.relative_to(TARGET.parents[3])} differs from msime-cloud {args.ref}; run scripts/sync-client-dialect.py and fix what the server tests then report', file=sys.stderr)
            return 1
        print(f'client_dialect.json matches msime-cloud {args.ref}')
        return 0
    TARGET.write_bytes(upstream)
    print(f'Wrote {TARGET.relative_to(TARGET.parents[3])} from msime-cloud {args.ref}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
