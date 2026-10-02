#!/usr/bin/env python3
"""Refresh, or with --check verify, server/tests/data/client_dialect.json against msime.

The file is msime's crates/client-core/src/skin/catalog/client_dialect.json: the case table for the skin manifest rules every platform reads skin.toml with, which the cross-platform client, msime-cloud's community library and this repository are all held to. The server test candidate_skin_catalog_loads_exactly_the_packages_the_client_accepts runs every case, so a rule changed there reaches this repository as a failing check rather than as skins that load on one platform and not on another.
"""
import argparse
import sys
import urllib.request
from pathlib import Path

SOURCE = 'https://raw.githubusercontent.com/metasequoiaime/msime/{ref}/crates/client-core/src/skin/catalog/client_dialect.json'
TARGET = Path(__file__).resolve().parents[1] / 'server/tests/data/client_dialect.json'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--ref', default='develop', help='msime branch, tag or commit to read (default: develop)')
    parser.add_argument('--check', action='store_true', help='fail when the copy differs instead of rewriting it')
    args = parser.parse_args()
    with urllib.request.urlopen(SOURCE.format(ref=args.ref), timeout=30) as response:
        upstream = response.read()
    if args.check:
        if TARGET.read_bytes() != upstream:
            print(f'{TARGET.relative_to(TARGET.parents[3])} differs from msime {args.ref}; run scripts/sync-client-dialect.py and fix what the server tests then report', file=sys.stderr)
            return 1
        print(f'client_dialect.json matches msime {args.ref}')
        return 0
    TARGET.write_bytes(upstream)
    print(f'Wrote {TARGET.relative_to(TARGET.parents[3])} from msime {args.ref}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
