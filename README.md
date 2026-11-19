ShonDJ

ShonDJ is my custom build of Mixxx, the open-source DJ software. I forked it to build the features I wanted for playing live sets: a phone remote, guest song requests, automatic stems, and a full custom skin.

Default branch: tips. The original Mixxx README and license are kept below.

What I added

Guest song requests (QR code) Guests scan a QR code and send song requests through a small relay server. ShonDJ polls the relay and shows a requests inbox next to the library, matches requests to songs I already have, and lets me drag them straight onto a deck. It also supports a 30-second crowd poll between two songs. Nothing plays on its own; I always decide.

Phone remote ShonDJ serves a control page over local Wi-Fi (scan a QR code to open it). If the phone drops off Wi-Fi, the page falls back to the relay over cell data and switches back when Wi-Fi returns.

Song import with automatic stems Drop files into an inbox folder and ShonDJ processes them in the background: it calls a Python script that runs Demucs to split each song into stems, adds the stem version to the library, and analyzes it. A two-lane queue keeps imports moving, and stem jobs pause while music is playing so the mix never stutters.

Practice mode Watches my transitions and scores each one after it ends: how tight the beats were, whether it started on a 16-bar phrase, and how long the blend ran. It only reports and never touches the mix.

Shon skin and interface

A full custom skin with its own layout, colors, toolbar, and waveform view
Info panel showing the selected song's BPM and key, tonight's crate, and waiting requests
Smoothed waveform rendering so peaks read as a clean wave
Stealth mode that hides song titles and cover art from people looking at the screen
Logo screen (Ctrl+L) that also appears after idle time, but never while a deck is playing
New ShonDJ logo, icons, and library icons

Library tool (Python) tools/shon_genres.py looks up each song on Apple's free music search and fills in the real genre, release year, and album cover. It has a preview mode and backs up the library database before writing.

Built with

C++ and Qt (inside the Mixxx codebase), Python, SQLite, and qrcodegen for the QR codes.

Screenshots
<img width="1917" height="1019" alt="Screenshot 2026-11-19 144451" src="https://github.com/user-attachments/assets/e2952b6f-d0bf-476c-aa79-c99a2116d44f" />
<img width="1917" height="1019" alt="Screenshot 2026-11-19 144353" src="https://github.com/user-attachments/assets/2fe98361-fd2d-48a4-8fed-ef0a0b1f135d" />
<img width="1917" height="1078" alt="Screenshot 2026-11-19 144248" src="https://github.com/user-attachments/assets/eaaeea58-707b-4f53-b62c-31d08ca00ab1" />

ShonDJ is based on Mixxx and is licensed under the GPL, the same as Mixxx. Everything below is the original Mixxx README.

# Mixxx

[![GitHub latest tag](https://img.shields.io/github/tag/mixxxdj/mixxx.svg)](https://mixxx.org/download)
[![Packaging status](https://repology.org/badge/tiny-repos/mixxx.svg)](https://repology.org/metapackage/mixxx/versions)
[![Build status](https://github.com/mixxxdj/mixxx/actions/workflows/build.yml/badge.svg)](https://github.com/mixxxdj/mixxx/actions/workflows/build.yml)
[![Coverage status](https://coveralls.io/repos/github/mixxxdj/mixxx/badge.svg)](https://coveralls.io/github/mixxxdj/mixxx)
[![Zulip chat](https://img.shields.io/badge/zulip-join_chat-brightgreen.svg)](https://mixxx.zulipchat.com)
[![Donate](https://img.shields.io/opencollective/all/mixxx?label=Donate)](https://mixxx.org/donate)

[Mixxx] is Free DJ software that gives you everything you need to perform live
DJ mixes. Mixxx works on GNU/Linux, Windows, and macOS.

## Quick Start

To get started with Mixxx:

1. For live use, [download the latest stable version][download-stable].
2. For experimentation and testing, [download a development release][download-testing].
3. To live on the bleeding edge, clone the repo: `git clone https://github.com/mixxxdj/mixxx.git`

## Bug tracker

The Mixxx team uses [Github Issues][issues] to manage Mixxx development.

Have a bug or feature request? [File a bug on Github][fileabug].

Want to get involved in Mixxx development? Assign yourself a bug from the [easy
bug list][easybugs] and get started!

## Building Mixxx

Read [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions, code style
guidelines, and how to open a pull request.

## Documentation

For help using Mixxx, there are a variety of options:

- [Mixxx manual][manual]
- [Mixxx wiki][wiki]
- [Hardware Compatibility]
- [Creating Skins]

## Translation

Help to spread Mixxx with translations into more languages, as well as to update and ensure the accuracy of existing translations.

- [Help translate content]
- [Mixxx i18n wiki]
- [Mixxx localization forum]
- [Mixxx glossary]

## Community

Mixxx is a vibrant community of hackers, DJs and artists. To keep track of
development and community news:

- Chat with us on [Zulip][zulip].
- Follow us on [Mastodon], [Bluesky] and [Facebook].
- Subscribe to the [Mixxx Blog][blog].
- Post on the [Mixxx forums][discourse].

## License

Mixxx is released under the GPLv2. See the LICENSE file for a full copy of the
license.

[mixxx]: https://mixxx.org
[download-stable]: https://mixxx.org/download/#stable
[download-testing]: https://mixxx.org/download/#testing
[issues]: https://github.com/mixxxdj/mixxx/issues
[fileabug]: https://github.com/mixxxdj/mixxx/issues/new/choose
[mastodon]: https://floss.social/@mixxx
[Bluesky]: https://bsky.app/profile/mixxx.bsky.social
[facebook]: https://www.facebook.com/pages/Mixxx-DJ-Software/21723485212
[blog]: https://mixxx.org/news/
[manual]: https://manual.mixxx.org/
[wiki]: https://github.com/mixxxdj/mixxx/wiki
[easybugs]: https://github.com/mixxxdj/mixxx/issues?q=is%3Aopen+is%3Aissue+label%3Aeasy
[creating skins]: https://mixxx.org/wiki/doku.php/Creating-Skins
[help translate content]: https://explore.transifex.com/mixxx-dj-software/
[Mixxx i18n wiki]: https://github.com/mixxxdj/mixxx/wiki/Internationalization
[Mixxx localization forum]: https://mixxx.discourse.group/c/translation/13
[hardware compatibility]: https://manual.mixxx.org/2.3/en/hardware/manuals.html
[zulip]: https://mixxx.zulipchat.com/
[discourse]: https://mixxx.discourse.group/
