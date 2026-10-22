"""ShonDJ: fill in real genre, release year and album covers from Apple Music search.

YouTube downloads come tagged "Music", which is useless for sorting. This looks
each song up by artist and title on Apple's free search (the same one the guest
page uses for covers) and fills in genre and year. It also saves the real album
cover (1000x1000) to Music\\Covers and points the song's cover there. Your audio
and stem files are never touched.

Close ShonDJ first, then run from C:\\dev\\mixxx:
    python tools\\shon_genres.py           (preview only, changes nothing)
    python tools\\shon_genres.py --apply   (backs up your library, then writes)
    add --no-covers to skip the album covers
"""
import datetime
import os
import difflib
import json
import re
import shutil
import sqlite3
import sys
import time
import urllib.parse
import urllib.request

DB = r"C:\dev\mixxx-settings\mixxxdb.sqlite"
COVERS = os.path.expanduser(r"~\Music\Covers")
JUNK_GENRES = {"", "music", "people & blogs", "entertainment", "film & animation", "none"}
MIN_SCORE = 0.62


def clean(text):
    text = text or ""
    text = re.sub(r"\(.*?\)|\[.*?\]|「.*?」", " ", text)          # (Official Video), [Remix]...
    text = re.sub(r"\b(ft|feat|featuring)\.?\b.*", " ", text, flags=re.I)
    text = re.sub(r"\b(official|video|audio|lyrics?|hd|4k|remaster(ed)?)\b", " ", text, flags=re.I)
    text = re.sub(r"[^\w\s&']", " ", text)
    return re.sub(r"\s+", " ", text).strip().lower()


def main_artist(artist):
    artist = re.split(r",|&| x |\bft\b|\bfeat\b|\(", artist or "", flags=re.I)[0]
    return clean(artist)


def similar(a, b):
    return difflib.SequenceMatcher(None, a, b).ratio()


def lookup(artist, title):
    term = f"{main_artist(artist)} {clean(title)}".strip()
    url = "https://itunes.apple.com/search?" + urllib.parse.urlencode(
        {"term": term, "entity": "song", "limit": 15, "country": "US"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=15) as r:
                return json.load(r).get("results", [])
        except Exception as e:
            if attempt == 2:
                print(f"   lookup failed: {e}")
                return []
            time.sleep(10)  # Apple rate limit, wait and retry


def best_match(artist, title, results):
    want_t, want_a = clean(title), main_artist(artist)
    scored = []
    for r in results:
        t = clean(r.get("trackName", ""))
        a = clean(r.get("artistName", ""))
        score = 0.6 * similar(want_t, t) + 0.4 * max(similar(want_a, a), 1.0 if want_a and want_a in a else 0)
        scored.append((score, r))
    if not scored:
        return None, 0
    top = max(s for s, _ in scored)
    if top < MIN_SCORE:
        return None, top
    # Among near-best matches, take the earliest release (skips remasters and compilations).
    close = [r for s, r in scored if s >= top - 0.05]
    close.sort(key=lambda r: r.get("releaseDate", "9999"))
    return close[0], top


def save_cover(hit, tid, label):
    art = hit.get("artworkUrl100", "")
    if not art:
        return None
    url = re.sub(r"\d+x\d+bb", "1000x1000bb", art)
    name = re.sub(r'[<>:"/\\|?*]', "", label)[:80].strip()
    path = os.path.join(COVERS, f"{tid} {name}.jpg")
    try:
        with urllib.request.urlopen(url, timeout=20) as r, open(path, "wb") as f:
            f.write(r.read())
        return path
    except Exception as e:
        print(f"   cover download failed: {e}")
        return None


def main():
    apply = "--apply" in sys.argv
    con = sqlite3.connect(DB)
    rows = con.execute(
        "SELECT id, artist, title, genre, year FROM library WHERE mixxx_deleted = 0 ORDER BY artist").fetchall()
    covers_on = "--no-covers" not in sys.argv
    if apply and covers_on:
        os.makedirs(COVERS, exist_ok=True)
    updates, covers, misses = [], [], []
    for i, (tid, artist, title, genre, year) in enumerate(rows, 1):
        needs_tags = (genre or "").strip().lower() in JUNK_GENRES or not (year or "").strip()
        if not needs_tags and not covers_on:
            continue
        hit, score = best_match(artist, title, lookup(artist, title))
        label = f"{artist or '?'} - {title}"
        if not hit:
            misses.append(label)
            print(f"[{i}/{len(rows)}] ??  {label}  (no confident match)")
        else:
            g = hit.get("primaryGenreName", "")
            y = (hit.get("releaseDate") or "")[:4]
            if needs_tags:
                updates.append((g, y, tid))
            if covers_on:
                if apply:
                    path = save_cover(hit, tid, label)
                    if path:
                        covers.append((path, tid))
                elif hit.get("artworkUrl100"):
                    covers.append((None, tid))
            print(f"[{i}/{len(rows)}] ok  {label}  ->  {g}, {y}   ({hit.get('artistName')} - {hit.get('trackName')})")
        time.sleep(3.2)  # Apple allows about 20 searches a minute

    print(f"\n{len(updates)} genres/years and {len(covers)} covers found, "
          f"{len(misses)} songs need you to fill in by hand.")
    if not apply:
        print("Preview only. Run again with --apply to save.")
        return
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    con.close()
    shutil.copy(DB, DB.replace(".sqlite", f".backup-{stamp}-genres.sqlite"))
    con = sqlite3.connect(DB)
    con.executemany("UPDATE library SET genre = ?, year = ? WHERE id = ?", updates)
    # Cover from a file you picked (source 2 = user selected, type 2 = file).
    # The image digest is left empty and a per-track placeholder hash keeps
    # covers apart; ShonDJ fills in the real digest when it loads the song.
    con.executemany(
        "UPDATE library SET coverart_source = 2, coverart_type = 2, coverart_location = ?, "
        "coverart_hash = (? % 65000) + 1, coverart_digest = NULL, coverart_color = NULL "
        "WHERE id = ?",
        [(path, tid, tid) for path, tid in covers])
    con.commit()
    con.close()
    print("Saved. Open ShonDJ: Genre, Year and covers are updated.")


if __name__ == "__main__":
    main()
