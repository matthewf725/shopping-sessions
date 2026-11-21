"""Check exception counts and selections against physical rows in known order."""
from bisect import bisect_left
from pathlib import Path
import random
import subprocess
import sys
import tempfile


def check(executable, data, flags, rng):
    header = 'event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents\n'
    hit = '"e12345678",u12345678,2024-01-02T00:15:00+01:00,2024-01-01T23:15:00Z,view,p,'
    miss = hit.replace('01T23:15', '02T23:15')
    split = len(flags) // 3
    for name, part in [('a', flags[:split]), ('b', []), ('c', flags[split:])]:
        (data / 'events' / f'{name}.csv').write_text(header + '\n'.join(miss if x else hit for x in part))
    rows = [i for i, exception in enumerate(flags) if exception]
    n = len(flags)
    ranges = [(0, n), (0, 0), (n, n)]
    for boundary in (1, 63, 64, 65, 127, 128, 129):
        if boundary <= n:
            ranges.extend([(boundary-1, boundary), (boundary, n)])
    ranges.extend(tuple(sorted((rng.randrange(n+1), rng.randrange(n+1)))) for _ in range(500))

    def run(mode, queries):
        inputs = ''.join(' '.join(map(str, q)) + '\n' for q in queries)
        result = subprocess.run([executable, data, *mode], input=inputs, text=True,
                                capture_output=True, check=True)
        return result.stdout.splitlines()

    expected_counts, selections, expected_rows = [], [], []
    for first, stop in ranges:
        start = bisect_left(rows, first)
        count = bisect_left(rows, stop) - start
        expected_counts.append(str(count))
        for k in {0, count//2, max(0, count-1), count}:
            selections.append((first, stop, k))
            expected_rows.append(str(rows[start+k]) if k < count else 'none')
    assert run([], ranges) == expected_counts
    assert run(['--select'], selections) == expected_rows
    listing = run(['--list'], [(0, n)])
    assert [int(line.split('\t')[0]) for line in listing[1:]] == rows


if __name__ == '__main__':
    executable = Path(sys.argv[1]).resolve()
    rng = random.Random(1301)
    with tempfile.TemporaryDirectory() as path:
        data = Path(path)
        (data / 'events').mkdir()
        (data / 'manifest.json').write_text(
            '{"window_start":"2024-01-01T00:00:00Z","window_end":"2024-01-04T00:00:00Z"}')
        for flags in ([], [False]*128, [True]*64, [True]*900+[False]*4000,
                      [False]*4000+[True]*900, [rng.random()<0.16 for _ in range(5000)]):
            check(executable, data, flags, rng)
    print('Exact counts, selections, and listings for all six layouts.')
