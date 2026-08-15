// Cart Ridge global leaderboard API (Cloudflare Worker + D1).
//
// Three separate leaderboards, one per immersion level (BASE/MEDIUM/FULL --
// see source/main.c's Immersion enum), since the levels aren't comparable:
// FULL is a much harder run than BASE, so lumping them into one ranked
// list would just mean BASE runs (worth less per-kill/per-wave, per the
// game's own scoring multiplier) never show up near the top.
//
// Routes:
//   POST /scores               submit a score (requires X-Cart-Ridge-Key)
//   GET  /scores                     -> { BASE: [...], MEDIUM: [...], FULL: [...] }
//   GET  /scores?immersion=BASE      -> [...]                (one leaderboard only)
//   GET  /scores?format=tsv          -> plain text, one row per line:
//                                        immersion\tname\tscore\twave\tkills
//                                        (no JSON parser needed on the 3DS side)
//   GET  /scores?limit=N              caps entries per leaderboard (default 10, max 50)

const IMMERSION_LEVELS = ["BASE", "MEDIUM", "FULL"];
const MAX_NAME_LEN = 12;
const MAX_SCORE = 100000000;

function clampLimit(url) {
	const raw = Number(url.searchParams.get("limit"));
	if (!Number.isFinite(raw) || raw <= 0) return 10;
	return Math.min(Math.floor(raw), 50);
}

async function topScores(db, immersion, limit) {
	const { results } = await db
		.prepare("SELECT name, score, wave, kills FROM scores WHERE immersion = ? ORDER BY score DESC LIMIT ?")
		.bind(immersion, limit)
		.all();
	return results;
}

async function handleGet(url, env) {
	const limit = clampLimit(url);
	const immersion = url.searchParams.get("immersion");
	const format = url.searchParams.get("format");

	if (immersion && !IMMERSION_LEVELS.includes(immersion)) {
		return new Response("Unknown immersion level", { status: 400 });
	}
	const levels = immersion ? [immersion] : IMMERSION_LEVELS;

	const byLevel = {};
	for (const level of levels) {
		byLevel[level] = await topScores(env.DB, level, limit);
	}

	if (format === "tsv") {
		let body = "";
		for (const level of levels) {
			for (const row of byLevel[level]) {
				body += `${level}\t${row.name}\t${row.score}\t${row.wave}\t${row.kills}\n`;
			}
		}
		return new Response(body, { headers: { "content-type": "text/plain" } });
	}

	return Response.json(immersion ? byLevel[immersion] : byLevel);
}

async function handlePost(request, env) {
	const key = request.headers.get("X-Cart-Ridge-Key");
	if (!env.SUBMIT_KEY || key !== env.SUBMIT_KEY) {
		return new Response("Unauthorized", { status: 401 });
	}

	let body;
	try {
		body = await request.json();
	} catch {
		return new Response("Bad JSON", { status: 400 });
	}

	const name = String(body.name ?? "").trim().slice(0, MAX_NAME_LEN);
	const score = Number(body.score);
	const wave = Number(body.wave) || 0;
	const kills = Number(body.kills) || 0;
	const immersion = String(body.immersion ?? "").toUpperCase();

	if (!name) return new Response("Missing name", { status: 400 });
	if (!Number.isFinite(score) || score < 0 || score > MAX_SCORE) {
		return new Response("Invalid score", { status: 400 });
	}
	if (!IMMERSION_LEVELS.includes(immersion)) {
		return new Response("Invalid immersion level", { status: 400 });
	}

	await env.DB
		.prepare("INSERT INTO scores (name, score, wave, kills, immersion) VALUES (?, ?, ?, ?, ?)")
		.bind(name, score, wave, kills, immersion)
		.run();

	return new Response("OK", { status: 201 });
}

export default {
	async fetch(request, env) {
		const url = new URL(request.url);
		if (url.pathname !== "/scores") return new Response("Not found", { status: 404 });

		if (request.method === "GET") return handleGet(url, env);
		if (request.method === "POST") return handlePost(request, env);
		return new Response("Method not allowed", { status: 405 });
	},
};
