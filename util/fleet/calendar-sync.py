#!/usr/bin/env python3
"""Publish Evolution's local calendar cache as the fleet calendar.

Reads the Evolution Data Server cache databases under
~/.cache/evolution/calendar/*/cache.db and writes:

    calendar/index          date-tile page
    calendar/<date>         day details
    calendar.status         summary line

This is a read-only bridge.  Evolution remains the CalDAV client; a
future version can replace it with a direct CalDAV sync.
"""
import datetime as dt
import glob
import os
import re
import sqlite3
import sys
from pathlib import Path
from zoneinfo import ZoneInfo

HERE = Path(__file__).resolve().parent
CACHE = Path(os.environ.get("EVOLUTION_CALENDAR_CACHE",
                            Path.home() / ".cache/evolution/calendar"))
DAYS = int(os.environ.get("WADA_CAL_DAYS", "14"))
TZNAME = os.environ.get("WADA_TZ")
if not TZNAME:
    try:
        TZNAME = Path("/etc/timezone").read_text().strip()
    except OSError:
        TZNAME = "UTC"
TZ = ZoneInfo(TZNAME)
UTC = dt.timezone.utc


def parse_stamp(s):
    if not s:
        return None
    try:
        return dt.datetime.strptime(s[:14], "%Y%m%d%H%M%S").replace(tzinfo=UTC)
    except ValueError:
        return None


def all_day(ics):
    m = re.search(r"DTSTART;VALUE=DATE:(\d{8})", ics or "")
    if not m:
        return None
    try:
        return dt.datetime.strptime(m.group(1), "%Y%m%d").date()
    except ValueError:
        return None


def events():
    out = []
    for db in sorted(glob.glob(str(CACHE / "*" / "cache.db"))):
        try:
            con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
            rows = con.execute(
                "select ECacheOBJ, occur_start, summary, location, status "
                "from ECacheObjects").fetchall()
            con.close()
        except sqlite3.Error as e:
            print(f"calendar-sync: {db}: {e}", file=sys.stderr)
            continue
        for ics, start, summary, location, status in rows:
            if status and status.lower() == "cancelled":
                continue
            summary = (summary or "").strip() or "(untitled)"
            location = (location or "").strip()
            when = parse_stamp(start)
            if when is not None:
                local = when.astimezone(TZ)
                out.append((local.date(), local.strftime("%H:%M"),
                            summary, location))
                continue
            day = all_day(ics)
            if day is not None:
                out.append((day, "All day", summary, location))
    # Keep one row per date/time/summary; caches can repeat instances.
    seen = set()
    uniq = []
    for e in sorted(out):
        key = (e[0], e[1], e[2])
        if key in seen:
            continue
        seen.add(key)
        uniq.append(e)
    return uniq


def main():
    today = dt.datetime.now(TZ).date()
    end = today + dt.timedelta(days=DAYS - 1)
    byday = {today + dt.timedelta(days=i): [] for i in range(DAYS)}
    total = 0
    for day, clock, summary, location in events():
        if day < today or day > end:
            continue
        byday[day].append((clock, summary, location))
        total += 1

    caldir = HERE / "calendar"
    caldir.mkdir(exist_ok=True)
    idx = ["schema=calendar", ""]
    for day in sorted(byday):
        items = byday[day]
        status = "free" if not items else (
            "1 event" if len(items) == 1 else f"{len(items)} events")
        idx += [
            f"date={day.isoformat()}",
            f"title={day.strftime('%a %d')}",
            f"status={status}",
            "",
        ]
        lines = []
        for clock, summary, location in items:
            line = f"{clock}  {summary}"
            if location:
                line += f"  ({location})"
            lines.append(line)
        text = "\n".join(lines) + "\n" if lines else "No events.\n"
        tmp = caldir / (day.isoformat() + ".tmp")
        tmp.write_text(text)
        os.replace(tmp, caldir / day.isoformat())

    index = HERE / "calendar/index.tmp"
    index.write_text("\n".join(idx).rstrip() + "\n")
    os.replace(index, HERE / "calendar/index")
    (HERE / "calendar.status.tmp").write_text(
        f"{DAYS} days · {total} events\n")
    os.replace(HERE / "calendar.status.tmp", HERE / "calendar.status")
    print(f"calendar-sync: {total} events across {DAYS} days "
          f"({TZNAME})")


if __name__ == "__main__":
    main()
