# Cart Ridge leaderboard backend

A small Cloudflare Worker + D1 (SQLite) API backing three separate global
leaderboards, one per immersion level (BASE / MEDIUM / FULL) -- the levels
aren't comparable in difficulty, so they're never ranked against each
other.

## Deploy (one-time setup)

You'll need `wrangler` (Cloudflare's CLI) and a Cloudflare account. From
this `leaderboard/` directory:

```bash
npm install -g wrangler   # if you don't already have it
wrangler login
```

Create the D1 database:

```bash
wrangler d1 create cart-ridge-leaderboard
```

That prints a `database_id` -- paste it into `wrangler.toml` in place of
`REPLACE_ME_AFTER_RUNNING_D1_CREATE`.

Apply the schema:

```bash
wrangler d1 execute cart-ridge-leaderboard --remote --file=./schema.sql
```

Set the submission key (anything long/random -- this is what stops
randoms from posting fake scores directly at the API; it needs to match
the `LEADERBOARD_SUBMIT_KEY` the game itself sends, see below):

```bash
wrangler secret put SUBMIT_KEY
```

Deploy:

```bash
wrangler deploy
```

This prints the Worker's URL (`https://cart-ridge-leaderboard.<your-subdomain>.workers.dev`).
That's the base URL the game's `httpc` calls need to point at.

## API

- `POST /scores` -- submit a score. Requires header `X-Cart-Ridge-Key:
  <the SUBMIT_KEY you set above>`. Body:
  ```json
  { "name": "MAT", "score": 12345, "wave": 7, "kills": 42, "immersion": "BASE" }
  ```
- `GET /scores` -- all three leaderboards at once:
  `{ "BASE": [...], "MEDIUM": [...], "FULL": [...] }`
- `GET /scores?immersion=BASE` -- just one leaderboard, as a plain array.
- `GET /scores?limit=N` -- cap entries per leaderboard (default 10, max 50).
- `GET /scores?format=tsv` -- plain-text alternative for the 3DS client,
  so it doesn't need a JSON parser: one line per entry,
  `immersion\tname\tscore\twave\tkills`.

## Testing it without the game

```bash
curl -X POST https://<your-worker-url>/scores \
  -H "X-Cart-Ridge-Key: <your key>" \
  -H "Content-Type: application/json" \
  -d '{"name":"MAT","score":12345,"wave":7,"kills":42,"immersion":"BASE"}'

curl https://<your-worker-url>/scores
curl "https://<your-worker-url>/scores?format=tsv"
```

## Local dev

```bash
wrangler dev
```

Runs against a local D1 instance -- apply the schema locally first with
`wrangler d1 execute cart-ridge-leaderboard --local --file=./schema.sql`.
