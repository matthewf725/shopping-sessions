"""Check independent accounting identities on a fresh seed without a published answer."""
from pathlib import Path
import sys
import polars as pl

folder = Path(sys.argv[1])
users = pl.scan_parquet(folder / 'user_daily/*.parquet')
campaigns = pl.scan_parquet(folder / 'campaign_daily/*.parquet')
counts = ('sessions', 'revenue_cents')
u = users.group_by('day').agg(pl.col(*counts).sum()).sort('day').collect()
c = campaigns.group_by('day').agg(pl.col(*counts).sum()).sort('day').collect()
assert u.equals(c), 'daily session or purchase-revenue totals disagree'
for table, key in ((users, 'user_id'), (campaigns, 'campaign_id')):
    duplicates = table.group_by(key, 'day').len().filter(pl.col('len') != 1).limit(1).collect()
    assert duplicates.is_empty(), 'duplicate output key'
bad = campaigns.filter((pl.col('purchasing_sessions') > pl.col('sessions')) |
                       ((pl.col('campaign_id') == 'organic') & (pl.col('assisted_sessions') != 0)) |
                       ((pl.col('campaign_id') != 'organic') & (pl.col('assisted_sessions') < pl.col('sessions')))).limit(1).collect()
assert bad.is_empty(), 'invalid campaign session counts'
print(f"Daily totals and unique keys agree across {u.height} days and {u['sessions'].sum():,} sessions.")
