# Oversized Pig

Play our strongest one-minute leaderboard bot in your browser. No account, GPU, API key, or third-party analytics. The engine and model run on your device; games save locally. On the hosted site, completed and resigned games are also uploaded automatically to private storage to improve the bot. Exit Game stops play and returns home; Resume Game restores your position, including after a reload. Resign ends the game with a win for Oversized Pig and preserves the replay and resignation result in your save.

**oversizedpig reached global #1 on Colonist’s 1v1 leaderboard.** The [leaderboard screenshot](web/leaderboard-2026-11-09.png) supplied November 9, 2026 shows a 2552 rating, 313 games, and 62.9% wins. These are account statistics from a snapshot, not a live ranking or the browser bot’s measured win rate. The profile URL and public-release license are pending. This project is independent of Colonist and the game publishers.

[Wei Hu](https://weihu.ca) · [Updates on X](https://x.com/_weihu_)

## Run locally

Download and unzip the ready-to-run archive from [Releases](https://github.com/TheWeiHu/colonizing-pig/releases), then run:

```sh
python3 serve.py
```

Open **http://localhost:8080**. Python 3.10+ serves static files only; all gameplay and inference happen in the browser. Safari 16.4+, current Chrome, Edge, or Firefox are required for WebAssembly SIMD. Opening the HTML with `file://` does not work.

## Build

Install [Emscripten 7.0.0](https://emscripten.org/docs/getting_started/downloads.html), activate its environment, then:

```sh
make test
make build
make serve
```

`dist/` is the whole website. Deploy it to Netlify with the included `_headers`; no paid inference service is needed. Any static host works: shared-memory headers enable up to four workers, while other hosts use one worker with the same search settings. Only the selected ~4.6 MB engine/model is downloaded. Hosting bandwidth and requests can still incur charges.

## Game recordings

The hosted game automatically submits its seed, moves, player seat, outcome, and pinned engine/model hashes to a small AWS collector. The payload contains no names, account identifiers, IP addresses, or browser fingerprint. Replays are private and marked unverified until their moves can be checked before training.

Uploads retry after connection failures without interrupting play. Up to 32 pending recordings are retained on the device; unavailable or full browser storage can prevent recovery after closing the page. The local game save is independent. Local builds and other hostnames do not upload. `scripts/replay_config.py` pins the exact engine and model in the build; `web/replay-config.json` contains the hosted origin and public endpoint.

## Model and rules

`model.json` pins **porto-ruling**, the source revision, full search specification, and SHA-256 hashes of its three evaluators. The engine retains 128 sampled worlds, eight finalists, and its three-turn rollout setting. Worker count changes throughput, not the search budget. Its recorded leaderboard time was 38.37 seconds per side per game using four cores; this is not a browser timing guarantee.

Two-player rules: random starting order, 15 points, balanced dice, discards above nine cards, friendly robber below three public points, and bank/port trades only. The bot sees its own player view, never the rival's hidden development cards or future randomness. Resource hands are trackable and visible. Local games are casual play, not tamper-proof ranked matches.

The model learns from self-play, scores positions with small neural networks, and compares candidate turns using sampled continuations. Training improved targets by removing measured luck and later taught the evaluator the search's preferred end-of-turn positions. Training data, experiment infrastructure, and historical models are intentionally outside this package.

## Verification

`make test` checks 24 golden games (62,452 positions), illegal actions, hidden-card views, and complete-game save/replay round trips. To compare native and browser decisions:

```sh
./build/test fixtures > build/fixtures.json
node tests/wasm.cjs single build/fixtures.json
node tests/wasm.cjs threaded build/fixtures.json
```

`make test-wasm` runs the golden rules and replay tests in WebAssembly. `npm ci && npm run test:browser` checks desktop Chromium and mobile WebKit after installing their Playwright browsers.

Tests use identical positions for equivalence only; these are not new playing-strength results. Browser testing should include threaded and fallback modes, mobile viewports, reloads, pause/resume, and slow devices. Full search is never silently reduced.
