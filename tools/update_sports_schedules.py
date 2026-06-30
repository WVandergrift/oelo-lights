#!/usr/bin/env python3
"""Build the static, normalized sports schedule feed used by controllers."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import time
import urllib.parse
import urllib.request
from pathlib import Path

LEAGUES = {
    "nfl": ("football", "nfl"),
    "nba": ("basketball", "nba"),
    "wnba": ("basketball", "wnba"),
    "mlb": ("baseball", "mlb"),
    "nhl": ("hockey", "nhl"),
}
BASE_URL = "https://site.api.espn.com/apis/site/v2/sports/{}/{}/scoreboard"


def slug(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")


def get_json(url: str, attempts: int = 3) -> dict:
    request = urllib.request.Request(
        url, headers={"User-Agent": "WVandergrift-oelo-lights-schedule-builder/1"}
    )
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return json.load(response)
        except Exception:
            if attempt + 1 == attempts:
                raise
            time.sleep(2**attempt)
    raise RuntimeError("unreachable")


def fetch_league(league: str, start: dt.date, end: dt.date) -> list[dict]:
    sport, endpoint = LEAGUES[league]
    events: dict[str, dict] = {}
    cursor = start
    while cursor <= end:
        # A month stays below ESPN's 1,000-event response cap even for MLB.
        chunk_end = min(cursor + dt.timedelta(days=27), end)
        params = urllib.parse.urlencode(
            {
                "dates": cursor.strftime("%Y%m%d")
                + "-"
                + chunk_end.strftime("%Y%m%d"),
                "limit": 1000,
            }
        )
        payload = get_json(BASE_URL.format(sport, endpoint) + "?" + params)
        for raw in payload.get("events", []):
            competition = (raw.get("competitions") or [{}])[0]
            competitors = competition.get("competitors") or []
            if len(competitors) != 2:
                continue
            teams = []
            for competitor in competitors:
                team = competitor.get("team") or {}
                name = team.get("displayName") or team.get("name")
                if not name:
                    break
                teams.append(
                    {
                        "key": "sports-teams-" + slug(name),
                        "name": name,
                        "abbreviation": team.get("abbreviation", ""),
                        "homeAway": competitor.get("homeAway", ""),
                    }
                )
            if len(teams) != 2:
                continue
            status = (competition.get("status") or {}).get("type") or {}
            season = raw.get("season") or {}
            event = {
                "id": f"{league}-{raw.get('id', '')}",
                "league": league,
                "start": raw.get("date", ""),
                "startEpoch": int(
                    dt.datetime.fromisoformat(
                        raw.get("date", "").replace("Z", "+00:00")
                    ).timestamp()
                ),
                "name": raw.get("shortName") or raw.get("name", "Game"),
                "seasonType": season.get("slug") or season.get("type", ""),
                "status": status.get("name", "STATUS_SCHEDULED"),
                "cancelled": status.get("name") in {
                    "STATUS_CANCELED",
                    "STATUS_POSTPONED",
                },
                "teams": teams,
            }
            if event["id"] and event["start"]:
                events[event["id"]] = event
        cursor = chunk_end + dt.timedelta(days=1)
    return sorted(events.values(), key=lambda event: event["start"])


def build(output: Path, start: dt.date, days: int) -> None:
    end = start + dt.timedelta(days=days)
    generated = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()
    all_events = []
    league_counts = {}
    for league in LEAGUES:
        events = fetch_league(league, start, end)
        league_counts[league] = len(events)
        all_events.extend(events)
    if not all_events:
        raise RuntimeError(f"all leagues returned no events for {start} through {end}")

    by_team: dict[str, dict] = {}
    for event in all_events:
        for team in event["teams"]:
            entry = by_team.setdefault(
                team["key"],
                {
                    "schema": 1,
                    "generatedAt": generated,
                    "team": {
                        "key": team["key"],
                        "name": team["name"],
                        "league": event["league"],
                        "abbreviation": team["abbreviation"],
                    },
                    "events": [],
                },
            )
            entry["events"].append(event)

    teams_dir = output / "teams"
    teams_dir.mkdir(parents=True, exist_ok=True)
    expected_files = set()
    for key, payload in sorted(by_team.items()):
        filename = key + ".json"
        expected_files.add(filename)
        (teams_dir / filename).write_text(
            json.dumps(payload, separators=(",", ":")) + "\n"
        )
    for old in teams_dir.glob("*.json"):
        if old.name not in expected_files:
            old.unlink()

    index = {
        "schema": 1,
        "generatedAt": generated,
        "range": {"start": start.isoformat(), "end": end.isoformat()},
        "source": "public scoreboards normalized through a SportsDataverse-compatible schema",
        "leagueEventCounts": league_counts,
        "teams": sorted(
            (payload["team"] for payload in by_team.values()),
            key=lambda team: (team["league"], team["name"]),
        ),
    }
    output.mkdir(parents=True, exist_ok=True)
    (output / "index.json").write_text(json.dumps(index, indent=2) + "\n")
    print(f"Published {len(all_events)} events for {len(by_team)} teams")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("docs/data/sports"))
    parser.add_argument("--start", type=dt.date.fromisoformat, default=dt.date.today())
    parser.add_argument("--days", type=int, default=370)
    args = parser.parse_args()
    build(args.output, args.start, args.days)


if __name__ == "__main__":
    main()
