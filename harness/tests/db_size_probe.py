"""Read-only probe: what is eating the nyaxus.db file size?"""
import sqlite3

con = sqlite3.connect("file:nyaxus.db?mode=ro", uri=True)
cur = con.cursor()

print("== schema ==")
for (name, sql) in cur.execute("SELECT name, sql FROM sqlite_master WHERE type='table'"):
    print(f"--- {name}")
    print(sql)

print("\n== page stats ==")
page_size = cur.execute("PRAGMA page_size").fetchone()[0]
page_count = cur.execute("PRAGMA page_count").fetchone()[0]
freelist = cur.execute("PRAGMA freelist_count").fetchone()[0]
print(f"page_size={page_size} pages={page_count} file_bytes={page_size * page_count} freelist_pages={freelist} ({freelist * page_size / 1048576:.1f} MB reclaimable by VACUUM)")

print("\n== table row counts + logical byte totals ==")
tables = [r[0] for r in cur.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'")]
for t in tables:
    cols = [c[1] for c in cur.execute(f"PRAGMA table_info({t})")]
    n = cur.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
    expr = "+".join(f"LENGTH(CAST({c} AS BLOB))" for c in cols)
    total = cur.execute(f"SELECT COALESCE(SUM({expr}), 0) FROM {t}").fetchone()[0]
    print(f"{t}: rows={n} logical_bytes={total} ({total / 1048576:.2f} MB)")

print("\n== events by type ==")
try:
    for row in cur.execute(
        "SELECT type, COUNT(*), COALESCE(SUM(LENGTH(CAST(payload AS BLOB))), 0) "
        "FROM events GROUP BY type ORDER BY 3 DESC"
    ):
        print(f"  {row[0]}: rows={row[1]} payload_bytes={row[2]} ({row[2] / 1048576:.2f} MB)")
except sqlite3.Error as e:
    print(f"  (events query failed: {e})")

print("\n== biggest single rows ==")
for t in tables:
    cols = [c[1] for c in cur.execute(f"PRAGMA table_info({t})")]
    idcol = "rowid"
    expr = "+".join(f"LENGTH(CAST({c} AS BLOB))" for c in cols)
    try:
        row = cur.execute(f"SELECT {idcol}, {expr} AS s FROM {t} ORDER BY s DESC LIMIT 3").fetchall()
        for r in row:
            print(f"  {t} rowid={r[0]}: {r[1]} bytes")
    except sqlite3.Error:
        pass

con.close()
