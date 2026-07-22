---
name: working-style-background-and-context
description: "How to work with this user — proactively fan out ≤3 background planning/recon agents ahead of time, and periodically advise when to start a fresh session"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 38343c62-84e6-420c-8476-b1c2611c38a7
---

Two standing working-style preferences the user set (2026-07-12), on the [[f2-rewrite-project]] but general:

1. **Proactively swarm background agents (max ~3) for planning/recon AHEAD OF TIME.**
   During any wait (a build, a review agent, an iteration), and before starting a
   new subsystem, spin up ≤3 read-only background agents to recon/plan the next
   pieces instead of working strictly serially. Flag it when doing so.

2. **Periodically tell the user whether to stop and start a fresh session / clear
   context**, so one session doesn't get over-polluted. Give this nudge from time
   to time — especially after a large landed milestone or before opening a big new
   subsystem.

**Why:** the user runs long, dense sessions and wants (a) latency hidden by
parallel background discovery — the 3-agent decouple sweep during the combat
build paid off big (caught the aiming UB + mapped the whole roadmap), and (b) to
manage context budget across sessions rather than blow one session up.

**How to apply:** default to launching background recon/plan agents proactively
(≤3 at a time) rather than asking whether to; keep them read-only. And when a
session has gotten large or a big new phase is starting, proactively recommend a
`/clear` or new session and make sure the resume state is captured in
[[f2-rewrite-project]] first.

3. **Keep it TLDR — be less chatty, especially at the START of a task** (user, 2026-07-14).
   Don't open by narrating a plan or restating context the user already has = noise.
   Open by ACTING. Save prose for genuine completion/decision points (finished a
   step/slice/head → a brief details drop is welcome). Default to dropping data the
   user shouldn't need to see.
