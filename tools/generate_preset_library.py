#!/usr/bin/env python3
"""Generate the built-in Leaf Lights preset library header."""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "firmware/tinys3/src/preset_library.h"
BASE_PRESETS = ROOT / "tools/fourth_of_july_presets.json"


def preset(name, category, colors, movement="march", speed=12, tags=()):
    rgb = []
    for color in colors:
        color = color.lstrip("#")
        rgb.extend(int(color[i : i + 2], 16) for i in (0, 2, 4))
    return {
        "name": name,
        "category": category,
        "tags": list(tags),
        "presetKey": re.sub(r"[^a-z0-9]+", "-", f"{category}-{name}".lower()).strip("-"),
        "builtin": True,
        "type": movement,
        "num_colors": len(colors),
        "direction": "F",
        "speed": speed,
        "gap": 0,
        "pause": 0,
        "other": 0,
        "colors": ",".join(map(str, rgb)) + ",",
    }


collections = [
    ("Holidays", [
        ("New Year's Countdown", ["FFD700", "FFFFFF", "C0C0C0"], "twinkle", 18, ["new year", "winter"]),
        ("Valentine Hearts", ["FF1744", "FF5C8A", "FFFFFF"], "fade", 9, ["valentine", "love"]),
        ("Mardi Gras", ["6F2DA8", "FFD700", "138808"], "march", 15, ["mardi gras"]),
        ("St. Patrick's Parade", ["009A44", "63C132", "FFFFFF", "FF8200"], "chase", 14, ["st patrick"]),
        ("Easter Morning", ["FFB3C6", "BDE0FE", "CAFFBF", "FFF1A8", "D8B4FE"], "blend", 8, ["easter", "spring"]),
        ("Cinco de Mayo", ["006847", "FFFFFF", "CE1126"], "march", 15, ["cinco de mayo"]),
        ("Memorial Day", ["B22234", "FFFFFF", "3C3B6E"], "river", 10, ["memorial day", "patriotic"]),
        ("Juneteenth", ["E31B23", "FFFFFF", "0057B8", "FFD700"], "takeover", 12, ["juneteenth"]),
        ("Canada Day", ["FF0000", "FFFFFF", "FF0000"], "march", 14, ["canada day"]),
        ("Labor Day", ["B22234", "FFFFFF", "3C3B6E"], "blend", 9, ["labor day", "patriotic"]),
        ("Oktoberfest", ["1D5D9B", "FFFFFF", "F7C948"], "shuffle", 12, ["oktoberfest", "fall"]),
        ("Halloween Haunt", ["FF6A00", "7B2CBF", "101010", "39FF14"], "twinkle", 16, ["halloween", "fall"]),
        ("Candy Corn", ["FF7A00", "FFD23F", "FFFFFF"], "fill", 10, ["halloween"]),
        ("Veterans Day", ["B22234", "FFFFFF", "3C3B6E"], "stationary", 10, ["veterans day", "patriotic"]),
        ("Thanksgiving Table", ["A44200", "E6A817", "7A3E00", "D95D39", "F6E7CB"], "blend", 7, ["thanksgiving", "fall"]),
        ("Hanukkah Nights", ["0038B8", "FFFFFF", "8EC5FF", "C0C0C0"], "twinkle", 10, ["hanukkah", "winter"]),
        ("Christmas Classic", ["D90429", "138A36", "FFFFFF", "FFD700"], "march", 10, ["christmas", "winter"]),
        ("Candy Cane Chase", ["E60026", "FFFFFF"], "chase", 14, ["christmas"]),
        ("Kwanzaa", ["E31B23", "111111", "138A36"], "takeover", 10, ["kwanzaa", "winter"]),
        ("Winter Wonderland", ["FFFFFF", "BDEBFF", "5DADE2", "C0C0C0"], "sprinkle", 8, ["winter", "snow"]),
    ]),
    ("Special Occasions", [
        ("Birthday Confetti", ["FF3B30", "FFCC00", "34C759", "007AFF", "AF52DE"], "sprinkle", 17, ["birthday", "party"]),
        ("Birthday Candles", ["FF9500", "FFD60A", "FFFFFF", "FF375F"], "twinkle", 10, ["birthday"]),
        ("Wedding Day", ["FFFFFF", "FFF4D6", "F7CAD0", "D4AF37"], "blend", 6, ["wedding", "romance"]),
        ("Golden Anniversary", ["D4AF37", "FFF8DC", "B8860B"], "fade", 7, ["anniversary"]),
        ("Silver Anniversary", ["C0C0C0", "FFFFFF", "8C9AA8"], "fade", 7, ["anniversary"]),
        ("Baby Shower Pastel", ["A2D2FF", "FFC8DD", "FFF1A8", "CDEAC0"], "blend", 7, ["baby shower"]),
        ("Graduation Night", ["FFD700", "FFFFFF", "1A1A1A"], "march", 12, ["graduation"]),
        ("Pride Rainbow", ["E40303", "FF8C00", "FFED00", "008026", "004DFF", "750787"], "river", 13, ["pride"]),
        ("Housewarming Glow", ["FF9F1C", "FFD166", "FFF4D6"], "fade", 6, ["housewarming"]),
        ("Welcome Home", ["2EC4B6", "CBF3F0", "FFBF69", "FF9F1C"], "takeover", 10, ["welcome"]),
        ("Game Night", ["00F5D4", "00BBF9", "9B5DE5", "F15BB5"], "arcade", 18, ["games", "party"]),
        ("Movie Night", ["E50914", "111111", "FFD700"], "streak", 8, ["movie"]),
        ("Dance Party", ["FF00FF", "00FFFF", "7CFF00", "FF6600"], "shuffle", 19, ["dance", "party"]),
        ("Pool Party", ["00B4D8", "48CAE4", "90E0EF", "FFD166"], "river", 14, ["pool", "summer"]),
        ("Gender Reveal", ["4CC9F0", "F72585", "FFFFFF"], "takeover", 9, ["baby shower"]),
        ("Retirement Celebration", ["FFD166", "06D6A0", "118AB2", "FFFFFF"], "sprinkle", 12, ["retirement"]),
        ("School Dance", ["8338EC", "3A86FF", "FF006E", "FFBE0B"], "arcade", 17, ["school", "dance"]),
        ("Quiet Evening", ["FFB703", "FB8500", "8ECAE6"], "fade", 4, ["relax"]),
    ]),
    ("Seasons", [
        ("Spring Bloom", ["FFAFCC", "BDE0FE", "CDEAC0", "FFF1A8"], "blend", 7, ["spring"]),
        ("April Showers", ["4361EE", "4CC9F0", "BDE0FE", "FFFFFF"], "river", 9, ["spring", "rain"]),
        ("Garden Party", ["2D6A4F", "95D5B2", "FFD6A5", "FFADAD"], "sprinkle", 9, ["spring"]),
        ("Summer Sunset", ["FF006E", "FB5607", "FFBE0B", "8338EC"], "blend", 8, ["summer"]),
        ("Ocean Breeze", ["023E8A", "0077B6", "00B4D8", "90E0EF"], "river", 8, ["summer", "ocean"]),
        ("Tropical Night", ["00F5D4", "F15BB5", "FEE440", "9B5DE5"], "shuffle", 12, ["summer", "tropical"]),
        ("Autumn Leaves", ["9C2C13", "D95D39", "E6A817", "6B3E26"], "takeover", 8, ["fall"]),
        ("Harvest Moon", ["FFB703", "FB8500", "3D2B1F"], "fade", 5, ["fall"]),
        ("Cozy Bonfire", ["FF4800", "FF7900", "FFB700", "541212"], "twinkle", 11, ["fall", "fire"]),
        ("First Snow", ["FFFFFF", "E0F4FF", "A9D6E5", "89C2D9"], "sprinkle", 7, ["winter", "snow"]),
        ("Northern Lights", ["00F5D4", "00BBF9", "7209B7", "3A0CA3"], "blend", 7, ["winter"]),
        ("Fireside", ["FFBA08", "F48C06", "DC2F02", "6A040F"], "twinkle", 8, ["winter", "fire"]),
    ]),
]

leagues = {
    "NFL": [
        ("Arizona Cardinals", "97233F", "000000"), ("Atlanta Falcons", "A71930", "000000"), ("Baltimore Ravens", "241773", "9E7C0C"), ("Buffalo Bills", "00338D", "C60C30"),
        ("Carolina Panthers", "0085CA", "101820"), ("Chicago Bears", "0B162A", "C83803"), ("Cincinnati Bengals", "FB4F14", "000000"), ("Cleveland Browns", "311D00", "FF3C00"),
        ("Dallas Cowboys", "003594", "869397"), ("Denver Broncos", "FB4F14", "002244"), ("Detroit Lions", "0076B6", "B0B7BC"), ("Green Bay Packers", "203731", "FFB612"),
        ("Houston Texans", "03202F", "A71930"), ("Indianapolis Colts", "002C5F", "A2AAAD"), ("Jacksonville Jaguars", "006778", "D7A22A"), ("Kansas City Chiefs", "E31837", "FFB81C"),
        ("Las Vegas Raiders", "000000", "A5ACAF"), ("Los Angeles Chargers", "0080C6", "FFC20E"), ("Los Angeles Rams", "003594", "FFA300"), ("Miami Dolphins", "008E97", "FC4C02"),
        ("Minnesota Vikings", "4F2683", "FFC62F"), ("New England Patriots", "002244", "C60C30"), ("New Orleans Saints", "D3BC8D", "101820"), ("New York Giants", "0B2265", "A71930"),
        ("New York Jets", "125740", "FFFFFF"), ("Philadelphia Eagles", "004C54", "A5ACAF"), ("Pittsburgh Steelers", "FFB612", "101820"), ("San Francisco 49ers", "AA0000", "B3995D"),
        ("Seattle Seahawks", "002244", "69BE28"), ("Tampa Bay Buccaneers", "D50A0A", "34302B"), ("Tennessee Titans", "0C2340", "4B92DB"), ("Washington Commanders", "5A1414", "FFB612"),
    ],
    "NBA": [
        ("Atlanta Hawks", "E03A3E", "C1D32F"), ("Boston Celtics", "007A33", "BA9653"), ("Brooklyn Nets", "000000", "FFFFFF"), ("Charlotte Hornets", "1D1160", "00788C"),
        ("Chicago Bulls", "CE1141", "000000"), ("Cleveland Cavaliers", "860038", "FDBB30"), ("Dallas Mavericks", "00538C", "002B5E"), ("Denver Nuggets", "0E2240", "FEC524"),
        ("Detroit Pistons", "C8102E", "1D42BA"), ("Golden State Warriors", "1D428A", "FFC72C"), ("Houston Rockets", "CE1141", "000000"), ("Indiana Pacers", "002D62", "FDBB30"),
        ("LA Clippers", "C8102E", "1D428A"), ("Los Angeles Lakers", "552583", "FDB927"), ("Memphis Grizzlies", "5D76A9", "12173F"), ("Miami Heat", "98002E", "F9A01B"),
        ("Milwaukee Bucks", "00471B", "EEE1C6"), ("Minnesota Timberwolves", "0C2340", "78BE20"), ("New Orleans Pelicans", "0C2340", "C8102E"), ("New York Knicks", "006BB6", "F58426"),
        ("Oklahoma City Thunder", "007AC1", "EF3B24"), ("Orlando Magic", "0077C0", "C4CED4"), ("Philadelphia 76ers", "006BB6", "ED174C"), ("Phoenix Suns", "1D1160", "E56020"),
        ("Portland Trail Blazers", "E03A3E", "000000"), ("Sacramento Kings", "5A2D81", "63727A"), ("San Antonio Spurs", "C4CED4", "000000"), ("Toronto Raptors", "CE1141", "000000"),
        ("Utah Jazz", "002B5C", "F9A01B"), ("Washington Wizards", "002B5C", "E31837"),
    ],
    "WNBA": [
        ("Atlanta Dream", "E31837", "5091CD"), ("Chicago Sky", "5091CD", "F9A01B"),
        ("Connecticut Sun", "F05023", "0A2240"), ("Dallas Wings", "C4D600", "002B5C"),
        ("Golden State Valkyries", "702F8A", "000000"), ("Indiana Fever", "002D62", "E03A3E"),
        ("Las Vegas Aces", "C8102E", "000000"), ("Los Angeles Sparks", "552583", "FDB927"),
        ("Minnesota Lynx", "0C2340", "78BE20"), ("New York Liberty", "6ECEB2", "000000"),
        ("Phoenix Mercury", "201747", "E56020"), ("Portland Fire", "D22630", "633F33"),
        ("Seattle Storm", "2C5234", "FEE11A"), ("Toronto Tempo", "71C5E8", "72253D"),
        ("Washington Mystics", "002B5C", "E31837"),
    ],
    "MLB": [
        ("Arizona Diamondbacks", "A71930", "E3D4AD"), ("Atlanta Braves", "CE1141", "13274F"), ("Baltimore Orioles", "DF4601", "000000"), ("Boston Red Sox", "BD3039", "0C2340"),
        ("Chicago Cubs", "0E3386", "CC3433"), ("Chicago White Sox", "27251F", "C4CED4"), ("Cincinnati Reds", "C6011F", "000000"), ("Cleveland Guardians", "00385D", "E50022"),
        ("Colorado Rockies", "33006F", "C4CED4"), ("Detroit Tigers", "0C2340", "FA4616"), ("Houston Astros", "002D62", "EB6E1F"), ("Kansas City Royals", "004687", "BD9B60"),
        ("Los Angeles Angels", "BA0021", "003263"), ("Los Angeles Dodgers", "005A9C", "EF3E42"), ("Miami Marlins", "00A3E0", "EF3340"), ("Milwaukee Brewers", "12284B", "FFC52F"),
        ("Minnesota Twins", "002B5C", "D31145"), ("New York Mets", "002D72", "FF5910"), ("New York Yankees", "0C2340", "C4CED4"), ("Oakland Athletics", "003831", "EFB21E"),
        ("Philadelphia Phillies", "E81828", "002D72"), ("Pittsburgh Pirates", "FDB827", "27251F"), ("San Diego Padres", "2F241D", "FFC425"), ("San Francisco Giants", "FD5A1E", "27251F"),
        ("Seattle Mariners", "0C2C56", "005C5C"), ("St. Louis Cardinals", "C41E3A", "0C2340"), ("Tampa Bay Rays", "092C5C", "8FBCE6"), ("Texas Rangers", "003278", "C0111F"),
        ("Toronto Blue Jays", "134A8E", "E8291C"), ("Washington Nationals", "AB0003", "14225A"),
    ],
    "NHL": [
        ("Anaheim Ducks", "FC4C02", "B9975B"), ("Boston Bruins", "FFB81C", "000000"), ("Buffalo Sabres", "003087", "FFB81C"), ("Calgary Flames", "D2001C", "FAAF19"),
        ("Carolina Hurricanes", "CC0000", "000000"), ("Chicago Blackhawks", "CF0A2C", "000000"), ("Colorado Avalanche", "6F263D", "236192"), ("Columbus Blue Jackets", "002654", "CE1126"),
        ("Dallas Stars", "006847", "8F8F8C"), ("Detroit Red Wings", "CE1126", "FFFFFF"), ("Edmonton Oilers", "041E42", "FF4C00"), ("Florida Panthers", "041E42", "C8102E"),
        ("Los Angeles Kings", "111111", "A2AAAD"), ("Minnesota Wild", "154734", "A6192E"), ("Montreal Canadiens", "AF1E2D", "192168"), ("Nashville Predators", "FFB81C", "041E42"),
        ("New Jersey Devils", "CE1126", "000000"), ("New York Islanders", "00539B", "F47D30"), ("New York Rangers", "0038A8", "CE1126"), ("Ottawa Senators", "C52032", "C2912C"),
        ("Philadelphia Flyers", "F74902", "000000"), ("Pittsburgh Penguins", "FCB514", "000000"), ("San Jose Sharks", "006D75", "EA7200"), ("Seattle Kraken", "001628", "99D9D9"),
        ("St. Louis Blues", "002F87", "FCB514"), ("Tampa Bay Lightning", "002868", "FFFFFF"), ("Toronto Maple Leafs", "003E7E", "FFFFFF"), ("Utah Mammoth", "6CACE4", "010101"),
        ("Vancouver Canucks", "00205B", "00843D"), ("Vegas Golden Knights", "B4975A", "333F42"), ("Washington Capitals", "041E42", "C8102E"), ("Winnipeg Jets", "041E42", "7B303E"),
    ],
}


def main():
    existing = json.loads(BASE_PRESETS.read_text())
    patterns = []
    for item in existing:
        item.update({"category": "Holidays", "tags": ["fourth of july", "patriotic"], "builtin": True})
        item["presetKey"] = re.sub(r"[^a-z0-9]+", "-", "holidays-" + item["name"].lower()).strip("-")
        patterns.append(item)
    next_id = max((p["id"] for p in patterns), default=0) + 1
    for category, items in collections:
        for name, colors, movement, speed, tags in items:
            p = preset(name, category, colors, movement, speed, tags)
            p["id"] = next_id
            next_id += 1
            patterns.append(p)
    for league, teams in leagues.items():
        for name, primary, secondary in teams:
            p = preset(name, "Sports Teams", [primary, secondary, primary, secondary], "march", 13, [league.lower(), "sports"])
            p["id"] = next_id
            next_id += 1
            patterns.append(p)
    payload = json.dumps(patterns, separators=(",", ":"))
    OUTPUT.write_text(
        "// SPDX-License-Identifier: MIT\n#pragma once\n\n#include <Arduino.h>\n\n"
        f"// Generated by tools/generate_preset_library.py ({len(patterns)} presets).\n"
        f'const char kSeedPatterns[] PROGMEM = R"JSON({payload})JSON";\n'
    )
    print(f"Generated {len(patterns)} presets in {OUTPUT}")


if __name__ == "__main__":
    main()
