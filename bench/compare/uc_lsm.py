#!/usr/bin/env python3
"""Kvrocks' live SST files by compression (stdlib only; runs on the host against the server's data dir).

  uc_lsm.py DB_DIR    (DB_DIR = <data dir>/db, holding *.sst, LOG and LOG.old.*)

Why: Kvrocks' defaults (rocksdb.compression_start_level 2 with level_compaction_dynamic_level_bytes) leave the files of
the levels above the bottom uncompressed, and how much data sits there after a load depends on compaction timing, so
the same load can end at very different sizes. This tool says how much of a measured size is uncompressed.

Each live *.sst is matched to its table_file_creation event in RocksDB's LOG files (cf_name, table_properties.
compression); a file with no event left in the logs counts as "unknown". Prints JSON: {"sst_files", "sst_bytes"
(apparent sizes), "by_compression": {name: bytes}, "by_cf": {cf: {name: bytes}}, "uncompressed_share" (NoCompression
bytes over the bytes whose compression is known, null when none is), "unknown_share" (bytes of files whose creation
event has rotated out of the logs, over sst_bytes)}. Blob files (kvrocks-blob) are not counted. Exits 1 when DB_DIR
is not a directory.
"""
import glob
import json
import os
import sys

MARK = "EVENT_LOG_v1 "


def creations(db):
    """file_number -> (cf_name, compression) from every LOG file, oldest first."""
    out = {}
    logs = sorted(glob.glob(os.path.join(db, "LOG.old.*"))) + [os.path.join(db, "LOG")]
    for path in logs:
        if not os.path.exists(path):
            continue
        with open(path, errors="replace") as f:
            for line in f:
                i = line.find(MARK)
                if i < 0:
                    continue
                try:
                    e = json.loads(line[i + len(MARK):])
                except ValueError:
                    continue
                if e.get("event") == "table_file_creation" and "file_number" in e:
                    props = e.get("table_properties") or {}
                    out[e["file_number"]] = (e.get("cf_name", "unknown"), props.get("compression") or "unknown")
    return out


def summary(db):
    made = creations(db)
    by_comp, by_cf, files, total = {}, {}, 0, 0
    for name in sorted(os.listdir(db)):
        if not name.endswith(".sst"):
            continue
        try:
            num = int(name[:-4])
            size = os.path.getsize(os.path.join(db, name))
        except (ValueError, OSError):
            continue
        cf, comp = made.get(num, ("unknown", "unknown"))
        files += 1
        total += size
        by_comp[comp] = by_comp.get(comp, 0) + size
        cfd = by_cf.setdefault(cf, {})
        cfd[comp] = cfd.get(comp, 0) + size
    known = total - by_comp.get("unknown", 0)
    share = round(by_comp.get("NoCompression", 0) / known, 4) if known else None
    return {"sst_files": files, "sst_bytes": total, "by_compression": by_comp, "by_cf": by_cf,
            "uncompressed_share": share, "unknown_share": round(by_comp.get("unknown", 0) / total, 4) if total else None}


def main(argv):
    if len(argv) != 2 or not os.path.isdir(argv[1]):
        print("usage: uc_lsm.py DB_DIR (an existing directory)", file=sys.stderr)
        return 1
    print(json.dumps(summary(argv[1]), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
