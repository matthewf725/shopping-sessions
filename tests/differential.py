"""Compare the native executable against a chronological, brute-force reference."""
import argparse
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile

import polars as pl

DAY = 86_400_000
GAP = 1_800_000
WEEK = 7 * DAY
EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)
ISO = re.compile(rb'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:Z|[+-](?:[01]\d|2[0-3]):[0-5]\d)')


def timestamp(text):
    if text.isdigit():
        digits = text.lstrip(b'0') or b'0'
        return int(digits) if len(digits) <= 19 else None
    if ISO.fullmatch(text):
        try:
            delta = datetime.fromisoformat(text.decode().replace('Z', '+00:00')) - EPOCH
            return delta.days * DAY + delta.seconds * 1000
        except ValueError:
            return None
    return None


def csv_rows(path, columns):
    files = path.glob('*.csv') if path.is_dir() else [path]
    for file in files:
        with file.open('rb') as stream:
            next(stream, None)
            for line in stream:
                fields = line.removesuffix(b'\n').split(b',')
                if len(fields) != columns:
                    continue
                fields = [f[1:-1] if len(f) >= 2 and f.startswith(b'"') and f.endswith(b'"') else f for f in fields]
                if all(b'"' not in f for f in fields):
                    yield fields


def reference(data):
    manifest = json.loads((data / 'manifest.json').read_text())
    start, end = (timestamp(manifest[k].encode()) for k in ('window_start', 'window_end'))
    products = dict(csv_rows(data / 'products.csv', 2))
    campaigns = dict(csv_rows(data / 'campaigns.csv', 2))
    campaigns[b'organic'] = b'organic'
    graph = defaultdict(set)
    for a, b in csv_rows(data / 'links', 2):
        if a and b:
            graph[a].add(b)
            graph[b].add(a)
    person = {}
    for a in graph:
        if a in person:
            continue
        seen, stack = {a}, [a]
        while stack:
            for b in graph[stack.pop()] - seen:
                seen.add(b)
                stack.append(b)
        smallest = min(seen)
        person.update(dict.fromkeys(seen, smallest))

    copies = {}
    for eid, user, ts, ingest, kind, product, price in csv_rows(data / 'events', 7):
        if eid and (eid not in copies or ingest < copies[eid][0]):
            copies[eid] = (ingest, user, ts, kind, product, price)
    events = defaultdict(list)
    for ingest, user, raw, kind, product, price in copies.values():
        ts = timestamp(raw)
        if not user or ts is None or not start <= ts < end or timestamp(ingest) - ts > DAY or not product:
            continue
        if kind not in (b'view', b'cart', b'purchase'):
            continue
        cents = 0
        if kind == b'purchase':
            if not price.isdigit():
                continue
            digits = price.lstrip(b'0') or b'0'
            if len(digits) > 8 or not 1 <= (cents := int(digits)) <= 10_000_000:
                continue
        events[person.get(user, user)].append((ts, cents, products.get(product, b'unknown')))
    clicks = defaultdict(set)
    for cid, user, campaign, raw in csv_rows(data / 'clicks', 4):
        ts = timestamp(raw)
        if cid and user and campaign in campaigns and campaign != b'organic' and ts is not None and start-WEEK <= ts < end:
            clicks[person.get(user, user)].add((ts, cid, campaign))

    users, totals = {}, defaultdict(lambda: [0, 0, 0, 0])
    for who, rows in events.items():
        sessions = []
        for row in sorted(rows):
            if not sessions or row[0] - sessions[-1][-1][0] > GAP:
                sessions.append([])
            sessions[-1].append(row)
        for session in sessions:
            t = session[0][0]
            day = (EPOCH + timedelta(milliseconds=t)).date()
            revenue = sum(e[1] for e in session)
            user = users.setdefault((who, day), [0, 0, 0, Counter()])
            user[0] += 1
            user[1] += len(session)
            user[2] += revenue
            for _, cents, cat in session:
                if cents:
                    user[3][cat] += cents
            active = [c for c in clicks[who] if t-WEEK <= c[0] <= t]
            credited = min(active, key=lambda c: (-c[0], c[1]))[2] if active else b'organic'
            total = totals[credited, day]
            total[0] += 1
            total[1] += bool(revenue)
            total[2] += revenue
            for campaign in {c[2] for c in active}:
                totals[campaign, day][3] += 1
    user_rows = [(who.decode(), day, s, e, r, min(c, key=lambda k: (-c[k], k)).decode() if c else None)
                 for (who, day), (s, e, r, c) in users.items()]
    camp_rows = [(c.decode(), campaigns[c].decode(), d, *tot) for (c, d), tot in totals.items()]
    return {'user_daily': user_rows, 'campaign_daily': camp_rows}


def adversarial(data, years):
    # Each session has repeated timestamps. Many campaign ranges span a machine word.
    r = random.Random(23)
    begin = datetime(2024, 1, 1, tzinfo=timezone.utc)
    base = (begin - EPOCH).days * DAY
    iso = lambda t: (EPOCH + timedelta(milliseconds=t)).strftime('%Y-%m-%dT%H:%M:%SZ')
    stop = base + (365*years if years else 14) * DAY
    data.mkdir()
    (data / 'manifest.json').write_text(json.dumps({'window_start': iso(base), 'window_end': iso(stop)}))
    (data / 'products.csv').write_text('product_id,category\np1,zzz\np2,aaa\n')
    (data / 'campaigns.csv').write_text('campaign_id,channel\nc_1,search\nc_2,email\nc_01,display\n')
    long_id = '夜' * 80
    alias = 'u\x00extra'
    links = [('u1', long_id), (long_id, alias), ('u1', 'A')]
    events, clicks = [], []
    eid = 0
    for j in range(182):
        ts = base + j * (GAP + 1001)
        if years and j > 89:
            ts += (365*years - 7)*DAY
        for person in (long_id, 'u1', alias):
            eid += 1
            events.append((f'e{eid}', person, str(ts), iso(ts), 'purchase', 'p1' if eid%2 else 'p2', '00071'))
        for k, ct in enumerate((ts, ts, ts+1, ts-WEEK, ts-WEEK-1)):
            clicks.append((f'k{k}-{j}', r.choice(['u1', alias, long_id]), r.choice(['c_1','c_2','c_01']), str(ct)))
        clicks.append(clicks[-1])
    for i, offset in enumerate((0, GAP, GAP*2+1, 3*GAP+1, 3*GAP+2, DAY-1, DAY, DAY+GAP)):
        ts = base + offset
        events.append((f'ex{i}', 'other', str(ts), iso(ts), 'view', 'q', 'bad'))
    # Compacted records must preserve ingestion precedence even outside the compact date range.
    events.extend([
        ('efar', 'other', str(base), '1970-01-01T00:00:00Z', 'view', 'p1', ''),
        ('efar', 'u1', str(base), iso(base), 'purchase', 'p2', '900'),
        ('ebad', 'other', 'broken', '1970-01-01T00:00:00Z', 'view', 'p1', ''),
        ('ebad', 'u1', str(base), iso(base), 'purchase', 'p2', '500'),
        ('elate', 'other', str(base), '2140-01-02T00:00:00Z', 'view', 'p1', ''),
    ])
    # Keep distinct seven- and eight-byte suffixes, including at the 24 h + 1 ms cutoff.
    events.extend([
        ('e1234567', 'u1234567', str(base + DAY), iso(base + 2*DAY), 'purchase', 'p1', '101'),
        ('e12345670', 'u12345670', str(base + DAY - 1), iso(base + 2*DAY), 'purchase', 'p2', '102'),
        ('e12345671', 'u12345670', str(base + DAY), iso(base + DAY), 'view', 'p1', ''),
    ])
    # Ingest ordering across a clock-digit carry and across months with the same day number.
    for eid, earlier, later in [('ecarry', '2024-01-01T09:59:59Z', '2024-01-01T10:00:00Z'),
                                ('emonth', '2023-12-01T23:59:59Z', '2024-01-01T00:00:00Z')]:
        events.append((eid, 'u1234567', str(base), earlier, 'purchase', 'p1', '103'))
        events.append((eid, 'u12345670', str(base), later, 'view', 'p2', ''))
    data_sets = {'events': ('event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents', events),
                 'clicks': ('click_id,user_id,campaign_id,ts', clicks), 'links': ('user_id_a,user_id_b', links)}
    for name, (header, rows) in data_sets.items():
        (data / name).mkdir()
        r.shuffle(rows)
        for i in range(3):
            with (data / name / f'{i}.csv').open('w', encoding='utf8', newline='') as f:
                print(header, file=f)
                for row in rows[i::3]:
                    print(','.join('"'+v+'"' if r.randrange(2) else v for v in row), file=f)


def chunk_boundaries(data):
    data.mkdir()
    start = '2025-01-01T00:00:00Z'
    (data / 'manifest.json').write_text(json.dumps({'window_start': start, 'window_end': '2025-01-02T00:00:00Z'}))
    (data / 'products.csv').write_text('product_id,category\np1,books\n')
    (data / 'campaigns.csv').write_text('campaign_id,channel\nc_1,email\n')
    for folder in ('events', 'clicks', 'links'):
        (data / folder).mkdir()
    header = 'event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents\n'
    for i, delta in enumerate((-1, 0, 1)):
        first = f'e{i}a,u1,{start},{start},view,p1,'
        tail = f'e{i}b,u1,{start},{start},purchase,p1,091'
        pad = 'x' * (512 * 1024 + delta - len(header) - len(first))
        giant = f'e{i}c,u1,{start},{start},view,p1,' + 'x' * (2 * 512 * 1024 + 7)
        (data / 'events' / f'{i}.csv').write_text(header + first + pad + '\n' + giant + '\n' + tail)


def check(exe, data, output):
    subprocess.run([exe, data, output], env=dict(os.environ, NCPU='3'), check=True)
    for table, expected in reference(data).items():
        actual = pl.read_parquet(output / table / '*.parquet').rows()
        assert Counter(actual) == Counter(expected), f'{data.name}: {table} differs'
    print(f'exact match: {data.name}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pipeline', type=Path)
    parser.add_argument('--generator', type=Path, help='also check six independent organizer-generated seeds')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as path:
        root = Path(path)
        for years in (0, 12):
            data = root / ('wide' if years else 'dense')
            adversarial(data, years)
            check(args.pipeline.resolve(), data, root / (data.name+'-out'))
        data = root / 'chunk-boundaries'
        chunk_boundaries(data)
        check(args.pipeline.resolve(), data, root / 'chunk-out')
        if args.generator:
            for seed in (0, 3, 11, 19, 47, 2147483661):
                data = root / f'seed-{seed}'
                subprocess.run([args.generator.resolve(), '--seed', str(seed), '--events', '15000', '--clicks', '7000', '--output', data], check=True)
                check(args.pipeline.resolve(), data, root / (data.name+'-out'))
