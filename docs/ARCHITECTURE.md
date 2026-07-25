# Architecture flow — dedicated server ↔ viewers (DRAFT)

Draft sketch for orientation, not a spec. The specs of record are `MP_PROTOCOL.md` (wire),
`MP_PROPOSAL.md` (co-op), `PRESENTATION_RECORD_REPLAY_SPEC.md` (presentation seam).

---

## 1. The one-paragraph version

`f2_server` is the **simulation with the client cut off**. It links `f2_core` and *not*
`f2_client`, so every presentation call the sim still makes lands on a stub. State changes are
published through one virtual seam — `presenter()` — which on the server encodes to a byte
stream. The viewer is the **real Fallout 2 client with its sim demoted to a mirror**: it decodes
that stream into its own object list and lets the vanilla renderer draw it. Input goes back as
plain text verbs through a trust boundary.

Two directions, two completely different shapes:

```
   AUTHORITY  ─────────── binary event stream (broadcast) ──────────►  PRESENTATION
   (f2_server)                                                          (viewers)

   AUTHORITY  ◄────────── text verbs, per-session, gated ─────────────  INPUT
```

---

## 2. The link-level picture (why stubs exist at all)

```
        ┌──────────────────── f2_server ────────────────────┐   ┌──── fallout2-ce (viewer) ────┐
        │                                                   │   │                              │
        │   f2_core          the SIM                        │   │   f2_core   +   f2_client    │
        │   ├─ object.cc / map.cc / scripts.cc              │   │   (everything, incl. SDL)    │
        │   ├─ combat.cc / item.cc / critter.cc             │   │                              │
        │   └─ interpreter*.cc  (scripts run HERE)          │   │                              │
        │                                                   │   │                              │
        │   NOT LINKED: f2_client  ───────────┐             │   │                              │
        │                                     ▼             │   │                              │
        │   server_stubs.cc   every client symbol the sim   │   │                              │
        │     ├─ serverStubAbort()  loud — must never run   │   │                              │
        │     └─ benign no-op       chrome with no headless │   │                              │
        │                           meaning                 │   │                              │
        │                                                   │   │                              │
        │   EXCEPTIONS, linked on purpose:                  │   │                              │
        │     game_dialog.cc    runs authoritative procs    │   │                              │
        │     inventory_ui.cc   barter is authoritative sim │   │                              │
        └───────────────────────────────────────────────────┘   └──────────────────────────────┘
```

> ⚠ The exceptions are load-bearing. `game_dialog.cc` calls `inventoryOpenTrade`; while
> `inventory_ui.cc` was *not* linked, that call hit an aborting stub and **one merchant
> conversation killed the server**. Anything the sim genuinely needs must be linked or severed,
> never left on an abort.

---

## 3. Outbound: state → pixels

```
  sim mutates something
        │
        ▼
  ┌───────────────┐   per beat    ┌──────────────┐
  │ objectDelta   │◄──────────────│ server_loop  │   objectDeltaScan() diffs a per-object
  │ Scan()        │               │ (100ms tick) │   shadow: fid/rot/flags/inv-hash/hp/ap…
  └──────┬────────┘               └──────────────┘
         │
         │  also: explicit calls from the sim
         │  presenter()->objectCreated / Moved / Destroyed
         │  presenter()->consoleMessageStyled / sfxPlay / dialogNode / barterState …
         ▼
  ┌─────────────────────────────────────────────┐
  │  presenter()          THE SEAM (presenter.h)│   base class = all no-ops
  │  ├─ NullPresenter      goldens / probe      │
  │  ├─ NarratePresenter   F2_NARRATE text      │
  │  ├─ ClientPresenter    local singleplayer   │
  │  └─ NetworkPresenter   ◄── f2_server        │
  └────────────────────┬────────────────────────┘
                       │  encodes: [type][flags][len][payload]
                       ▼
              ┌──────────────────┐
              │ SocketByteSink   │  broadcast to every session; optional tee to file
              └────────┬─────────┘
                       │  TCP
   ════════════════════╪══════════════════════════════════════════════════
                       ▼
              ┌──────────────────┐
              │ client_net.cc    │  decode loop, switch on event type
              └────────┬─────────┘
                       │
      ┌────────────────┼────────────────────┬─────────────────────┐
      ▼                ▼                    ▼                     ▼
  object list     client_present.cc    client_dialog.cc      client_barter.cc
  (the MIRROR)    glide/replay FSM     dialog window         trade mirrors
      │                │                    │                     │
      └────────────────┴────────────────────┴─────────────────────┘
                       │
                       ▼
             vanilla renderer / windows / SDL
```

**Key rule:** the viewer never simulates. It applies state and plays presentation.

> ⚠ `NetworkPresenter` is installed **only when a socket is connected**. With no client the
> presenter is the base class and every emission is an empty virtual — the server runs perfectly
> and streams nothing. A test without a client proves nothing.

---

## 4. Inbound: input → authority

```
  player clicks / drags / presses a key
        │
        ▼
  viewer builds a TEXT VERB          "mv 17488 1"   "use 612"   "btake 259 1"   "dsay 0"
        │
        ▼  TCP, one line per verb
  ┌──────────────────────────────────────────────────────────────┐
  │ serverControlLine()      ►► THE TRUST BOUNDARY               │
  │   flood guard (lines/beat)                                   │
  │   session → registry SLOT → actor                            │
  │   unbound? dead? not your turn? not your trade?  → REFUSE    │
  │   ServerActorScope scope(actor)   gDude := the acting player │
  └───────────────┬──────────────────────────────────────────────┘
                  │
     ┌────────────┼─────────────┬──────────────┬─────────────────┐
     ▼            ▼             ▼              ▼                 ▼
  immediate   combat_intent  dialog_intent  barter_intent   worldmap_intent
  (mv, use)      queue          queue          queue            queue
     │            │              │              │                 │
     │            ▼              ▼              ▼                 ▼
     │      turn barrier    dialog pump     barter pump     wm driver
     │      (combat.cc)     (_gdProcess)  (inventoryOpen   (worldmapServer
     │                                       Trade)          Driver)
     └────────────┴──────────────┴──────────────┴─────────────────┘
                                 │
                                 ▼
                        the real engine function
                        (_obj_use, itemMoveForce, _combat_attack …)
```

**Every refusal answers the player** (`kMsgChannelRefusal`), not just the operator's stderr.

---

## 5. Modal flows (the block-and-pump pattern)

Vanilla modals are blocking loops. On the server they'd spin forever with no input; the pattern
is: **park the tick inside the modal and service the wire from within it.**

```
  script or player opens a modal
        │
        ▼
  ┌────────────────────────────────────────────────────────────┐
  │  BLOCKING LOOP (dialog / barter / movie / worldmap)        │
  │                                                            │
  │    emit current state ───────────────────────────► viewers │
  │                                                            │
  │    while (no intent queued):                               │
  │        pump():  drain inbound verbs                        │
  │                 bail? → quit / no clients / DRIVER GONE    │
  │        sleep                                               │
  │                                                            │
  │    apply the intent → mutate sim → emit new state          │
  └────────────────────────────────────────────────────────────┘
```

> ⚠ **The sim tick does not advance while a modal is open.** `objectDeltaScan` does not run, so
> *nothing* streams except what the modal explicitly emits. That is why dialog and barter each
> ship their own state events — without them a spectator sees a frozen world.

> ⚠ The bail condition must be **"the DRIVER is still connected"**, not "anyone is connected".
> With host-only drive, a second connected player kept the old predicate satisfied while nobody
> who could answer remained — the server span forever.

Viewer side, the mirror-image hazard: a viewer modal is *also* a blocking loop, so it starves
`conn.pump()`. Every modal must be in `kViewerModalMask`, which makes `viewerServiceTicker` pump
the wire from inside it and force-close on `COMBAT_ENTER`. **Adding a modal without adding it to
that mask stalls the wire.**

---

## 6. Worked example — barter, end to end

```
  P2 clicks TALK on Tubby
        │  "talk 868"
        ▼
  serverControlLine → walk-then-act latch → arrives adjacent
        │  scriptsRequestDialog(Tubby)  + latch WHO asked (slot 1)
        ▼
  scriptsHandleRequests → ServerScriptRequestHandler::dialogEnter()
        │  ServerActorScope scope(P2)      ◄── holds for the WHOLE conversation
        ▼
  gameDialogEnter → _gdProcess ──► EVENT_DIALOG_NODE ──────────► all viewers
        │                          (P2 editable, others read-only)
        │  P2 sends "dsay 0"  → the barter option
        ▼
  inventoryOpenTrade()
        │  EVENT_BARTER_BEGIN(merchant, driver=P2) ─────────────► all viewers
        │  EVENT_BARTER_STATE(tables, offer, asking) ───────────► all viewers
        │
        │  P2 drags an item          "btake 259 1"
        │       → barter_intent queue → itemMoveForce
        │       → EVENT_BARTER_STATE (re-sent, whole snapshot) ─► all viewers
        │                                                          "bam, item on the table"
        │  P2 presses Offer          "bcommit"
        │       → barterAttemptTransaction  (prices computed HERE,
        │         under P2's scope: party barter skill, Master Trader,
        │         script-set reaction — NOT derivable viewer-side)
        │       → EVENT_BARTER_STATE ──────────────────────────► all viewers
        ▼
  "bdone" → tables swept back → EVENT_BARTER_END ───────────────► all viewers
        │
        ▼  tick resumes → objectDeltaScan → OBJECT_DELTA_INVENTORY (the real transfer)
```

Note the two channels doing different jobs: `BARTER_STATE` is **what it looks like while open**
(presentation, snapshot, self-healing); `OBJECT_DELTA_INVENTORY` is **what actually moved**
(authoritative, lands when the tick resumes).

---

## 7. Worked example — combat

```
  server enters combat ──► EVENT_COMBAT_ENTER ──────────────────► all viewers
                                                                   (modals force-close)
  ┌── per turn ────────────────────────────────────────────────────────────┐
  │  whose turn?                                                            │
  │    AI      → resolved instantly server-side                             │
  │    player  → TURN BARRIER: block until that SLOT's intent arrives       │
  │              (a viewer refuses input while its queue is busy, so the    │
  │               barrier doubles as an IMPLICIT ACK — this is the only     │
  │               thing pacing emission today)                              │
  │                                                                         │
  │  intent → _combat_attack / move / item use                              │
  │       │                                                                 │
  │       ├─ sim result → OBJECT_DELTA (hp, ap, flags, inventory)           │
  │       └─ presentation → pres_record captures the sequence               │
  │                          → EVENT_PRES_SEQ → viewer REPLAYS it through   │
  │                            the real reg_anim engine (not re-derived)    │
  └─────────────────────────────────────────────────────────────────────────┘
```

> ⚠ Only the actor whose turn it is gets paced. A **dead** player is never waited on, so nothing
> throttles their queue — it grows until events are dropped and their view goes to soup. The fix
> of record is a server-side emission outbox (pace the emission, not the simulation).

---

## 8. Identity — the chain everything hangs off

```
   sessionId          dies with the socket
       │
       ▼
   registry SLOT      durable for the process; slot 0 = host = gDude
       │
       ▼
   netId              valid for ONE baseline generation
```

> ⚠ `objectAssignAllNetIds` **re-mints every netId on every rebaseline** (including when a second
> viewer joins). A stale netId does not go dead — it resolves to a *different object*. Never cache
> one. `EVENT_PLAYER_ROSTER` re-announces slot→netId after every baseline for exactly this reason.

`gDude` is a **per-client role**, not a global truth: on each viewer it points at that client's own
actor. Server-side, `ServerActorScope` makes it point at whoever is currently acting, which is what
makes "the acting player" mean the right thing down the whole callee tree.

---

## 9. Where things are

| concern | server | viewer |
|---|---|---|
| trust boundary / verbs | `server_control.cc` | — |
| tick, delta scan | `server_loop.cc` | — |
| stubs / severance | `server_stubs.cc` | — |
| animation applier | `server_anim.cc` | `animation.cc` (real) |
| presenter impl | `presenter_network.cc` | `presenter_client.cc` |
| wire decode | — | `client_net.cc` |
| glide / replay FSM | — | `client_present.cc` |
| dialog | `game_dialog.cc` (shared) | `client_dialog.cc` |
| barter | `inventory_ui.cc` (shared) | `client_barter.cc` |
| intents | `*_intent.cc` (f2_core, both sides) | |
