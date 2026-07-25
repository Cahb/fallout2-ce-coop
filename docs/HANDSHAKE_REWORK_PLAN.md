# Server-authoritative character-creation handshake — plan of record

Status: **PLAN, design owner-confirmed, not yet implemented** (2026-07-22). Revises the
client-initiated staging in `ACCOUNT_IDENTITY_DESIGN.md`. Anchors `file:line` on `rewrite/phase0`.

## Goal
Whether the creation screen shows is **server-authoritative**, never a client env flag.
- Client sends `login <name>` = IDENTITY ONLY (no pre-rolled spec, no local editor yet).
- Server replies `EVENT_ACCOUNT_STATUS(sessionId, status)`: `0=LOADED` (existing → resume, no
  creation) / `1=CREATE_REQUIRED` (new account).
- Client shows the editor ONLY on CREATE_REQUIRED → sends `create <spec>` (rolled) or bare
  `create` (ESC/abandon → server spawns default).
- FUTURE (not built): `status=2 REROLL_ALLOWED` lets an existing account re-enter creation.

## Current flow (client-driven — the problem)
Client (`main.cc`): editor runs BEFORE connect (`main.cc:1002-1082`), gated on
`F2_PLAYER_CREATE=ask/ui` (1025-1026), composes 12-int `createFromUi` (1069-1073). Connect 1105,
blob-wait 1116-1134. Sends `platform` 1205-1213, `create <spec>` 1227-1233, `login <name>`
1235-1242 (or bare `claim` 1244). Server (`server_control.cc:1365` serverControlLine): `create`
handler 1402-1451 stashes `gPendingCreateSpecs[sessionId]`; `login` handler 1453-1641 —
`accountSlotForName` 1480, existing→resume 1493-1522 (ignores spec), new→pick slot / latch
`PendingLogin` 1561-1599 / apply spec 1615-1628; greet `serverGreetClaimant` 1639. Flaw: a
returning player pointlessly rolls a character the server then discards.

## Wire addition
`EVENT_ACCOUNT_STATUS = 46` (next free; `EventType` in `presenter_network.cc:61-190` tops at
`EVENT_BARTER_END=45`; mirror in `client_net.cc:109-148`). Direction server→client. Fields:
`sessionId` (i32 — addressed by session, NOT netId: a new account has no actor yet; client filters
on `_mySessionId`), `status` (u8). Emitted in direct reply to the client's `login`, which the
client sends only after `blobLoaded()` — so it always arrives after the blob + initial roster.

## Implementation steps
Wire plumbing first (compiles, emits nothing):
1. `presenter_network.cc:61-190` — add `EVENT_ACCOUNT_STATUS=46`.
2. `presenter.h:68` — `virtual void accountStatus(int sessionId, int status) {}`.
3. `presenter_network.cc:~904` — override: `beginEvent(EVENT_ACCOUNT_STATUS,0); putI32(sessionId); putU8(status); endEvent();` (guard `presenterEmissionsSuppressed()`).
4. `client_net.cc:109-148` mirror enum; `:600-634` add `case EVENT_ACCOUNT_STATUS: onAccountStatus(r)`.
5. `client_net.cc` `onAccountStatus`: read sessionId/status; `if (sessionId != _mySessionId) return;` (idiom `:831`); latch (mirror `onInventoryGrant` `:753`).
6. `client_net.{h,cc}` — `AccountStatus takeAccountStatus()` one-shot consume (mirror `takeInventoryGrant`).

Server decision:
7. `server_control.cc:1493` resume branch — after bind, `presenter()->accountStatus(sessionId, LOADED)` (keep greet).
8. `server_control.cc:1523` new-account branch — emit `accountStatus(sessionId, CREATE_REQUIRED)`; do NOT bind/spawn default yet. Add `bool decisionResolved` to `PendingLogin` (1165-1171), push with `false`.
9. `server_control.cc:1402-1451` `create` handler — now arrives after login; attach spec to this session's unresolved PendingLogin, set `decisionResolved=true`. Add bare-`create` (0-arg) path → no spec, resolved → default.
10. `server_control.cc:1192-1291` `serverControlDrainPendingLogins` — skip PendingLogins with `decisionResolved==false`; rest unchanged.

Client reorder (`main.cc`):
11. Delete 1002-1082; move body into `static bool rollCharacterViaEditor(char* out, size_t cap)` (keep the cursor/palette bracket 1029-1046 + `_ResetPlayer` 1039 — live-bug fixes).
12. After `blobLoaded()`: always send `platform`; if `F2_PLAYER_NAME` set send `login <name> [token]` (NO create first); pump (reuse 1116-1134 keep-alive/renderPresent/ESC) until `takeAccountStatus()!=NONE`; LOADED→enter world; CREATE_REQUIRED→interactive `rollCharacterViaEditor` OR headless literal `F2_PLAYER_CREATE` spec→send `create <spec>` or bare `create`. `F2_PLAYER_NAME` unset → keep bare `claim` (no status round-trip).

## F2_PLAYER_CREATE split
`=ask`/`ui` → interactive (editor only if server says CREATE_REQUIRED). `=<numeric spec>` →
headless pre-supplied spec, applied iff CREATE_REQUIRED, ignored on LOADED. Both sent AFTER
login/reply now.

## Risks / gotchas
- **"Send nothing" isn't implementable** — server can't tell "editor open" from "abandoned".
  Always send a terminator: `create <spec>` or bare `create`. Event-driven, no timeout.
- **BIGGEST GOTCHA: editor vs MOVE_ON_TOP window.** Old code ran the editor BEFORE the
  presentation window (`main.cc:1091`) precisely because that `WINDOW_MOVE_ON_TOP` window sits
  above the editor (black screen, working sound — 1007-1013). Moving the editor after connect =
  it now runs while that window exists. Must hide/destroy the presentation window around the
  editor call (or open editor above it). Verify visually.
- netId/roster: reply addressed by sessionId (from preamble `client_net.cc:3069-3089`), NOT netId.
- Create-spec still applied AFTER body exists, BEFORE rebaseline (`server_control.cc:1258-1271`) —
  only *when it arrives from the client* changes.
- Goldens/gates drive bare `claim`, never `login` — account-status emitted only on the login path
  → byte-identical. Keep the greppable `"control claimed by session %d (slot %d)"` on both bind
  paths (1637, 1289). Don't emit account-status on `claim`.
- ~0.5s connecting beat (login→reply→editor-or-world); the pump loop must keep the window alive.

## ACCOUNT_IDENTITY_DESIGN.md revision (drop-in)
Add a "Join/create chain — SERVER-AUTHORITATIVE creation trigger (revises §5 stage-2)" section:
login = identity only; server replies EVENT_ACCOUNT_STATUS (id 46, by sessionId); LOADED→resume,
CREATE_REQUIRED→editor(interactive)/spec(headless)→`create`/bare-`create`; new login latches
awaiting-decision, resolved by `create`, spawned only at the safe drain, no timeout. Invariants:
existing accounts never re-roll; spec applied after body/before rebaseline; `claim` path
byte-identical, no status; `"control claimed by session"` on every bind path.
