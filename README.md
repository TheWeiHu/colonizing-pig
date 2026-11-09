# Colonizing Pig

**Can you beat Oversized Pig?** It reached #1 on Colonist’s 1v1 leaderboard ([November 9, 2026 snapshot](web/leaderboard-2026-11-09.png)).

[Play in your browser](https://colonizing-pig.weihu.ca) · [How it was built](https://weihu.ca/posts/building-top-ranked-catan-1v1-bot/)

The model runs entirely on your device. No account, GPU, or LLM calls. Every device uses the same search budget; slower devices take longer.

## Run locally

Download and unzip the ready-to-run archive from [Releases](https://github.com/TheWeiHu/colonizing-pig/releases), then:

```sh
python3 serve.py
```

Open [localhost:8080](http://localhost:8080) in a modern browser. Requires Python 3.10+.

## Build from source

Install and activate [Emscripten 7.0.0](https://emscripten.org/docs/getting_started/downloads.html), then:

```sh
make build
make serve
```

Deploy `dist/` to any static host. Run engine tests with `make test`. Model details are in [model.json](model.json).

Completed and resigned games on the hosted site are automatically saved privately to improve the bot. Local builds do not upload games.

Built by [Wei Hu](https://weihu.ca) · [Updates on X](https://x.com/_weihu_). Independent of Colonist and the game publishers.
