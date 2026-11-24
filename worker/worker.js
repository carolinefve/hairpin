// =====================================================================
//  VOICE DIARY WORKER  (Cloudflare Workers + Workers AI + KV)
// ---------------------------------------------------------------------
//  The hairpin uploads a recording in parts. When the last part arrives
//  the Worker joins them into ONE WAV, transcribes it, turns it into a
//  note, stores it, and pings your phone (Pushcut or ntfy) if set up.
//
//  Needs (set in the Cloudflare dashboard, not here):
//    NOTES        KV namespace binding
//    AI           Workers AI binding
//    DEVICE_KEY   secret, same value as in the hairpin sketch
//    PUSHCUT_URL  optional secret: iPhone phone delivery (Pushcut)
//    NTFY_URL     optional secret: Android phone delivery (ntfy)
// =====================================================================

const TRANSCRIBE_MODEL = "@cf/openai/whisper-large-v3-turbo";
const NOTE_MODEL = "@cf/meta/llama-3.3-70b-instruct-fp8-fast";
const TIMEZONE = "Europe/London";
const WAIT_FOR_MISSING_PARTS_HOURS = 6; // then make a note from what arrived

// ---- Prompt 3 from the guide, word for word. Do not edit. ----
const STRUCTURING_PROMPT = `Never translate. The note must be written in exactly the same language
as the transcript, word for word the same language, no exceptions.

You receive a raw voice-diary transcript. Turn it into a clean,
well-structured, visually rich note.

Rules:
- Write the note in exactly the same language as the transcript. Do not
  translate any part of it, do not switch languages, and do not mix in
  English unless the transcript itself is in English. Keep the meaning,
  tone, and first-person voice exactly as spoken. Do not add
  interpretation.
- Keep first person. This is the author's own diary, so preserve their
  voice. Clean up only filler words, false starts, and transcription
  noise.
- Never use dashes as punctuation. No em dashes, no " - " connectors.
  Use commas, periods, or split the sentence instead.
- No AI-sounding phrasing, no motivational cliches, no added
  interpretation or advice. Do not invent anything that is not in the
  transcript.
- Split the body into topics. For every distinct topic, start with a
  bold Markdown header line made of one emoji that actually fits that
  topic's content plus a short 2-4 word heading, for example
  "**🏃 Morning run**", then a blank line, then a short paragraph.
  Leave a blank line between topic blocks. Never reuse the same emoji
  for unrelated topics, and never use a generic placeholder emoji.
- If the author lists things within a topic, use a numbered list inside
  that topic's paragraph.
- If the author mentions tasks or intentions, extract them into a todos
  array as short imperative items, written in the same language as the
  transcript. If there are no tasks, todos must be an empty array.
- Beyond the topic headers, you may use **bold** for a word or phrase
  the author clearly emphasized, and *italics* for something the author
  is directly quoting. Use both sparingly.
- key_phrases: 3 to 5 short phrases actually present in or central to
  the transcript, in the same language as the transcript.
- summary: one sentence capturing the essence, in the same language as
  the transcript.

Respond with ONLY valid JSON, no markdown fences, matching exactly this
shape:
{ "body": "...", "todos": ["..."], "key_phrases": ["..."], "summary": "..." }

Reminder: never translate. The note, its todos, key phrases, and
summary must all be written in exactly the same language as the
transcript.`;

const NOTE_SCHEMA = {
  type: "object",
  properties: {
    body: { type: "string" },
    todos: { type: "array", items: { type: "string" } },
    key_phrases: { type: "array", items: { type: "string" } },
    summary: { type: "string" },
  },
  required: ["body", "todos", "key_phrases", "summary"],
};

// =====================================================================
//  ENTRY POINTS
// =====================================================================
export default {
  async fetch(request, env, ctx) {
    try {
      return await route(request, env, ctx);
    } catch (err) {
      console.error("Unhandled error:", err);
      return json(
        { error: String(err && err.message ? err.message : err) },
        500,
      ); // always JSON, never HTML
    }
  },
  // Hourly safety net (set the schedule in Settings > Triggers).
  async scheduled(event, env, ctx) {
    ctx.waitUntil(safetyNet(env, false).then((r) => console.log(r)));
  },
};

async function route(request, env, ctx) {
  const url = new URL(request.url);
  const path = url.pathname.replace(/\/+$/, "") || "/";
  const method = request.method;

  if (method === "GET" && path === "/setup") return html(SETUP_PAGE);

  if (
    !env.DEVICE_KEY ||
    request.headers.get("X-Device-Key") !== env.DEVICE_KEY
  ) {
    return json({ error: "unauthorized" }, 401);
  }
  if (method === "POST" && path === "/upload")
    return handleUpload(request, env, ctx);
  if (method === "GET" && path === "/notes/next") return nextNote(env, ctx);
  if (method === "GET" && path === "/notes") return debugNotes(env, url);
  if (method === "GET" && path === "/setup/status") return status(env);
  if (method === "POST" && path === "/setup/reprocess")
    return text(await safetyNet(env, true));
  return json({ error: "not found" }, 404);
}

// =====================================================================
//  UPLOAD: store each part; when all parts are in, build the note
// =====================================================================
async function handleUpload(request, env, ctx) {
  const bytes = new Uint8Array(await request.arrayBuffer());
  if (bytes.length === 0) return json({ error: "empty body" }, 400);
  if (
    bytes.length < 44 ||
    String.fromCharCode(...bytes.slice(0, 4)) !== "RIFF"
  ) {
    return json({ error: "not a WAV file" }, 400);
  }

  const filename =
    request.headers.get("X-Filename") || `NOTIME-${Date.now()}.wav`;
  const { session, part, total } = parseName(filename);

  // A late retry of a recording that's already been turned into a note.
  if (await env.NOTES.get(`done:${session}`))
    return json({ ok: true, info: "already processed" });

  await env.NOTES.put(chunkKey(session, part), bytes, {
    metadata: { total, at: Date.now() },
  });

  // Check by reading each part directly. KV's list() can lag behind fresh
  // writes by up to a minute, so it can't be trusted for this check.
  const complete =
    total > 0 && (await allPartsPresent(env, session, total, part));
  if (complete)
    ctx.waitUntil(
      finalize(env, session, false, total).then((r) => console.log(session, r)),
    );

  return json({ ok: true, session, part, total, complete });
}

async function allPartsPresent(env, session, total, justStored) {
  for (let i = 1; i <= total; i++) {
    if (i === justStored) continue;
    const s = await env.NOTES.get(chunkKey(session, i), "stream");
    if (!s) return false;
    await s.cancel(); // only checking it exists
  }
  return true;
}

// "261123-123548_p01of03.wav" -> session 261123-123548, part 1 of 3
function parseName(name) {
  const base = name.replace(/\.wav$/i, "");
  let m = base.match(/^(.+)_p(\d+)of(\d+)$/i);
  if (m) return { session: m[1], part: +m[2], total: +m[3] };
  m = base.match(/^(.+)_(\d{2})$/); // guide's older "_NN" shape, total unknown
  if (m) return { session: m[1], part: +m[2], total: 0 };
  return { session: base, part: 1, total: 1 }; // a single complete file
}

function chunkKey(session, part) {
  return `chunk:${session}:${String(part).padStart(2, "0")}`;
}

async function sessionState(env, session) {
  const { keys } = await env.NOTES.list({ prefix: `chunk:${session}:` });
  const meta = keys.map((k) => k.metadata || {});
  const total = Math.max(0, ...meta.map((m) => m.total || 0));
  return {
    keys,
    total,
    complete: total > 0 && keys.length >= total,
    newest: Math.max(0, ...meta.map((m) => m.at || 0)),
    oldest: Math.min(Date.now(), ...meta.map((m) => m.at || Date.now())),
  };
}

// =====================================================================
//  FINALIZE: join -> transcribe -> structure -> store -> notify
// =====================================================================
async function finalize(env, session, force, knownTotal = 0) {
  // Claim it so two runs can't build the same note twice.
  if (await env.NOTES.get(`lock:${session}`)) return "already being processed";
  await env.NOTES.put(`lock:${session}`, String(Date.now()), {
    expirationTtl: 900,
  });

  try {
    // Which part keys to read: by number when we know the total, else by listing.
    let names,
      oldest = Date.now();
    if (knownTotal) {
      names = Array.from({ length: knownTotal }, (_, i) =>
        chunkKey(session, i + 1),
      );
    } else {
      const state = await sessionState(env, session);
      if (!state.complete && !force) return "waiting for more parts";
      names = state.total
        ? Array.from({ length: state.total }, (_, i) =>
            chunkKey(session, i + 1),
          )
        : state.keys.map((k) => k.name).sort(); // zero-padded, so order is right
    }

    const parts = [];
    let missing = 0;
    for (const n of names) {
      const { value, metadata } = await env.NOTES.getWithMetadata(
        n,
        "arrayBuffer",
      );
      if (!value) {
        missing++;
        continue;
      }
      parts.push(new Uint8Array(value));
      if (metadata && metadata.at) oldest = Math.min(oldest, metadata.at);
    }
    if (!parts.length) return "no audio";
    if (missing && !force) return `waiting for ${missing} more part(s)`;

    const wav = joinWavs(parts); // ONE file before transcribing
    const when = sessionTime(session, oldest);
    const { transcript, language } = await transcribe(env, wav);

    if (transcript) {
      const note = await structure(env, transcript, language);
      await storeNote(env, note, when, session, transcript);
      await notifyPhone(env);
    }

    for (const n of names) await env.NOTES.delete(n);
    await env.NOTES.put(`done:${session}`, "1", {
      expirationTtl: 7 * 24 * 3600,
    });
    return transcript ? "note created" : "no speech found, no note made";
  } catch (err) {
    console.error(`Finalize failed for ${session}:`, err);
    return `failed: ${err.message || err}`;
  } finally {
    await env.NOTES.delete(`lock:${session}`); // always release the claim
  }
}

// Keep the first 44-byte header, append every part's audio, fix the sizes.
function joinWavs(parts) {
  const audioBytes = parts.reduce((sum, p) => sum + p.length - 44, 0);
  const out = new Uint8Array(44 + audioBytes);
  out.set(parts[0].subarray(0, 44), 0);
  let offset = 44;
  for (const p of parts) {
    out.set(p.subarray(44), offset);
    offset += p.length - 44;
  }
  const view = new DataView(out.buffer);
  view.setUint32(4, 36 + audioBytes, true); // RIFF size
  view.setUint32(40, audioBytes, true); // data size
  return out;
}

// "261123-123548" is UK local time; convert to a real instant (handles BST).
function sessionTime(session, fallbackMs) {
  const m = session.match(/^(\d{2})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})$/);
  if (!m) return new Date(fallbackMs); // NOTIME-...: use when it arrived
  const asIfUtc = Date.UTC(2000 + +m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]);
  let utc = asIfUtc - zoneOffset(asIfUtc);
  utc = asIfUtc - zoneOffset(utc); // second pass for the changeover hour
  return new Date(utc);
}

function zoneOffset(ms) {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone: TIMEZONE,
    hourCycle: "h23",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).formatToParts(new Date(ms));
  const get = (t) => +parts.find((p) => p.type === t).value;
  const local = Date.UTC(
    get("year"),
    get("month") - 1,
    get("day"),
    get("hour"),
    get("minute"),
    get("second"),
  );
  return local - Math.floor(ms / 1000) * 1000;
}

// =====================================================================
//  AI
// =====================================================================
async function transcribe(env, wav) {
  const res = await env.AI.run(TRANSCRIBE_MODEL, {
    audio: toBase64(wav),
    task: "transcribe",
    vad_filter: true, // skips silence, fewer made-up words
  });
  return {
    transcript: (res.text || "").trim(),
    language: (res.transcription_info && res.transcription_info.language) || "",
  };
}

function toBase64(bytes) {
  if (typeof bytes.toBase64 === "function") return bytes.toBase64();
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) {
    s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  }
  return btoa(s);
}

async function structure(env, transcript, langCode) {
  let language = langCode;
  try {
    language =
      new Intl.DisplayNames(["en"], { type: "language" }).of(langCode) ||
      langCode;
  } catch {}
  if (!language) language = "the same language as the transcript";

  const messages = [
    { role: "system", content: STRUCTURING_PROMPT },
    {
      role: "user",
      content: `Detected transcript language: ${language}. Write the note in this exact language, do not translate.\n\n${transcript}`,
    },
  ];

  let out;
  try {
    const r = await env.AI.run(NOTE_MODEL, {
      messages,
      temperature: 0.3,
      max_tokens: 2048,
      response_format: { type: "json_schema", json_schema: NOTE_SCHEMA },
    });
    out = r.response;
  } catch (err) {
    // JSON mode can refuse ("JSON Mode couldn't be met"); ask again without it.
    const r = await env.AI.run(NOTE_MODEL, {
      messages,
      temperature: 0.3,
      max_tokens: 2048,
    });
    out = r.response;
  }
  if (typeof out === "string") {
    out = JSON.parse(
      out
        .trim()
        .replace(/^```(?:json)?\s*/i, "")
        .replace(/\s*```$/, ""),
    );
  }
  if (
    !out ||
    typeof out.body !== "string" ||
    !Array.isArray(out.todos) ||
    !Array.isArray(out.key_phrases) ||
    typeof out.summary !== "string"
  ) {
    throw new Error("AI reply was missing body, todos, key_phrases or summary");
  }
  return {
    body: out.body,
    todos: out.todos.map(String).filter((t) => t.trim()),
    key_phrases: out.key_phrases.map(String).filter((t) => t.trim()),
    summary: out.summary,
  };
}

// =====================================================================
//  NOTES: a linked list, newest first, plus one pointer key
// =====================================================================
async function getPtr(env) {
  return (
    (await env.NOTES.get("ptr:notes", "json")) || {
      latestId: null,
      deliveredId: null,
      lastSig: "",
    }
  );
}

// Re-read just before writing so two writers don't undo each other.
async function updatePtr(env, changes) {
  const ptr = await getPtr(env);
  Object.assign(ptr, changes);
  await env.NOTES.put("ptr:notes", JSON.stringify(ptr));
  return ptr;
}

async function storeNote(env, n, when, session, transcript) {
  const iso = when.toISOString();
  const id = `note:${iso}::${session}`;
  const ptr = await getPtr(env);
  const note = {
    id,
    prevId: ptr.latestId === id ? null : ptr.latestId,
    title: "Today's Diary",
    dateLine: new Intl.DateTimeFormat("en-US", {
      timeZone: TIMEZONE,
      weekday: "long",
      month: "long",
      day: "numeric",
      year: "numeric",
    }).format(when), // e.g. "Friday, August 21, 2026"
    body: markdownToUnicode(n.body),
    todos: n.todos,
    key_phrases: n.key_phrases,
    summary: n.summary,
    transcript, // exactly what Whisper heard, unedited
    filename: session,
    iso,
    createdAt: new Date().toISOString(),
  };
  await env.NOTES.put(id, JSON.stringify(note)); // note first...
  await updatePtr(env, { latestId: id }); // ...then move the pointer
  return note;
}

// GET /notes/next : one note as plain text, or an empty string. No list().
async function nextNote(env, ctx) {
  const ptr = await getPtr(env);
  if (!ptr.latestId || ptr.latestId === ptr.deliveredId) return text("");

  // Walk back from the newest note to the last one delivered.
  const waiting = [];
  let id = ptr.latestId;
  while (id && id !== ptr.deliveredId && waiting.length < 50) {
    const n = await env.NOTES.get(id, "json");
    if (!n) break;
    waiting.push(n);
    id = n.prevId;
  }
  waiting.reverse(); // oldest undelivered first

  let lastSig = ptr.lastSig || "";
  for (let i = 0; i < waiting.length; i++) {
    const n = waiting[i];
    const body = (n.body || "").trim();
    const summary = (n.summary || "").trim();
    const sig = `${body}|${summary}`;
    if (!body || !summary || sig === lastSig) {
      // empty or duplicate: skip it for good
      await updatePtr(env, { deliveredId: n.id });
      continue;
    }
    await updatePtr(env, { deliveredId: n.id, lastSig: sig });
    // More notes still queued? Ping the phone again a few seconds from now,
    // so each tap brings the next one until the queue is empty.
    if (i < waiting.length - 1) {
      ctx.waitUntil(
        new Promise((r) => setTimeout(r, 5000)).then(() => notifyPhone(env)),
      );
    }
    return text(render(n));
  }
  await updatePtr(env, { deliveredId: ptr.latestId });
  return text("");
}

// =====================================================================
//  LOOK OF THE NOTE (matches the screenshot)
// =====================================================================
function render(n) {
  const lines = [
    `☁️ ${bold("Today's Diary")} ☁️`,
    n.dateLine,
    "",
    n.body.trim(),
  ];
  if (n.todos && n.todos.length) {
    lines.push("", `✅ ${bold("To Do")}`);
    for (const t of n.todos) lines.push(`☐ ${t}`);
  }
  lines.push(
    "",
    `🔑 ${bold("Key Phrases")}: ${n.key_phrases.join(", ")}`,
    `📝 ${bold("Summary")}: ${n.summary}`,
  );
  if (n.transcript) lines.push("", `🎙️ ${bold("Original")}`, n.transcript);
  return lines.join("\n");
}

// Notes apps don't show Markdown, so **bold** and *italic* become real styled letters.
function markdownToUnicode(s) {
  return s
    .replace(/\*\*([^*\n]+?)\*\*/g, (_, t) => bold(t))
    .replace(/\*([^*\n]+?)\*/g, (_, t) => italic(t));
}

function mapLetters(s, upper, lower, digit) {
  return Array.from(s)
    .map((c) => {
      const x = c.codePointAt(0);
      if (x >= 65 && x <= 90) return String.fromCodePoint(upper + x - 65);
      if (x >= 97 && x <= 122) return String.fromCodePoint(lower + x - 97);
      if (digit && x >= 48 && x <= 57)
        return String.fromCodePoint(digit + x - 48);
      return c;
    })
    .join("");
}
const bold = (s) => mapLetters(s, 0x1d400, 0x1d41a, 0x1d7ce);
const italic = (s) =>
  mapLetters(s, 0x1d434, 0x1d44e, 0).replace(/\u{1d455}/gu, "ℎ");

// =====================================================================
//  PHONE PING (Pushcut), only if PUSHCUT_URL is set
// =====================================================================
async function notifyPhone(env) {
  // Android: ntfy. Only says "New diary note"; the note itself is never sent here.
  if (env.NTFY_URL) {
    try {
      const r = await fetch(env.NTFY_URL, {
        method: "POST",
        headers: { Title: "New diary note" },
        body: "Voice diary",
      });
      console.log("ntfy ping:", r.status);
    } catch (err) {
      console.error("ntfy ping failed:", err);
    }
  }
  if (!env.PUSHCUT_URL) return;
  try {
    // No title or text sent: custom text is a Pushcut Pro feature. The free
    // version shows the notification exactly as you set it up in the app.
    const r = await fetch(env.PUSHCUT_URL, { method: "POST" });
    console.log("Pushcut ping:", r.status);
  } catch (err) {
    console.error("Pushcut ping failed:", err);
  }
}

// =====================================================================
//  SAFETY NET (hourly) and manual "process now"
// =====================================================================
async function safetyNet(env, manual) {
  const sessions = new Set();
  let cursor;
  do {
    const r = await env.NOTES.list({ prefix: "chunk:", cursor });
    for (const k of r.keys)
      sessions.add(k.name.slice(6, k.name.lastIndexOf(":")));
    cursor = r.list_complete ? null : r.cursor;
  } while (cursor);

  const report = [];
  for (const s of sessions) {
    const st = await sessionState(env, s);
    const quietMs = Date.now() - st.newest;
    const quietLimit =
      manual || !st.total
        ? 3 * 60 * 1000
        : WAIT_FOR_MISSING_PARTS_HOURS * 3600 * 1000;
    const force = quietMs > quietLimit;
    if (st.complete || force)
      report.push(`${s}: ${await finalize(env, s, force)}`);
    else
      report.push(`${s}: waiting (${st.keys.length}/${st.total || "?"} parts)`);
  }
  return report.length ? report.join("\n") : "Nothing waiting.";
}

// =====================================================================
//  DEBUG + SETUP PAGE (these may use list(); the phone never calls them)
// =====================================================================
async function debugNotes(env, url) {
  const ptr = await getPtr(env);
  const notes = [];
  let id = ptr.latestId;
  while (id && notes.length < 20) {
    const n = await env.NOTES.get(id, "json");
    if (!n) break;
    notes.push(n);
    id = n.prevId;
  }
  if (url.searchParams.get("consume") === "1" && ptr.latestId)
    await updatePtr(env, { deliveredId: ptr.latestId });
  return json({ pointer: ptr, notes });
}

async function status(env) {
  const count = async (prefix) => {
    let n = 0,
      cursor;
    do {
      const r = await env.NOTES.list({ prefix, cursor });
      n += r.keys.length;
      cursor = r.list_complete ? null : r.cursor;
    } while (cursor);
    return n;
  };
  return json({
    notes: await count("note:"),
    audio_parts_waiting: await count("chunk:"),
    pointer: await getPtr(env),
  });
}

const SETUP_PAGE = `<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Voice diary</title>
<style>body{font-family:-apple-system,sans-serif;max-width:640px;margin:24px auto;padding:0 16px}
input{width:100%;padding:10px;font-size:16px;margin:6px 0 12px;box-sizing:border-box}
button{padding:10px 14px;font-size:15px;margin:0 8px 8px 0}
pre{background:#f4f4f4;padding:12px;white-space:pre-wrap;word-break:break-word}</style></head>
<body><h2>Voice diary</h2>
<label>Device key</label><input id="k" type="password" placeholder="paste your device key">
<button onclick="go('GET','/setup/status')">Check status</button>
<button onclick="go('POST','/setup/reprocess')">Process waiting audio now</button>
<button onclick="go('GET','/notes')">Show latest notes</button>
<pre id="o">Results appear here.</pre>
<script>
async function go(method, path) {
  const o = document.getElementById('o');
  o.textContent = 'Working...';
  try {
    const r = await fetch(path, { method, headers: { 'X-Device-Key': document.getElementById('k').value } });
    o.textContent = 'HTTP ' + r.status + '\\n\\n' + await r.text();
  } catch (e) { o.textContent = 'Error: ' + e; }
}
</script></body></html>`;

// =====================================================================
//  RESPONSE HELPERS
// =====================================================================
function json(obj, status = 200) {
  return new Response(JSON.stringify(obj, null, 2), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8" },
  });
}
function text(s, status = 200) {
  return new Response(s, {
    status,
    headers: { "Content-Type": "text/plain; charset=utf-8" },
  });
}
function html(s) {
  return new Response(s, {
    headers: { "Content-Type": "text/html; charset=utf-8" },
  });
}
