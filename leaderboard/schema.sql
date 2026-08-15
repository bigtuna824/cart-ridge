-- Cart Ridge global leaderboard schema (Cloudflare D1 / SQLite).
--
-- Three separate leaderboards live in one table, partitioned by
-- `immersion` (BASE / MEDIUM / FULL, matching the in-game Immersion enum)
-- rather than three separate tables -- a run only ever posts to the one
-- leaderboard matching whatever level was locked in for that run, and
-- querying "top N for immersion X" is the same query shape regardless of
-- which level, just filtered.

CREATE TABLE IF NOT EXISTS scores (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  name       TEXT NOT NULL,
  score      INTEGER NOT NULL,
  wave       INTEGER NOT NULL,
  kills      INTEGER NOT NULL,
  immersion  TEXT NOT NULL CHECK (immersion IN ('BASE', 'MEDIUM', 'FULL')),
  created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

-- Serves "top N for this immersion level" without a table scan.
CREATE INDEX IF NOT EXISTS idx_scores_immersion_score
  ON scores (immersion, score DESC);
