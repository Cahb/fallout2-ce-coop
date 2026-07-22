---
name: code-readability-policy
description: "Readability strategy for the RE-derived codebase — quarantine ugly names, GLOSSARY.md as decoder+rename-map, big rename pass POST-v1-leash (golden-safe), new seams always get real names."
metadata: 
  node_type: memory
  type: project
  originSessionId: 8b98e888-0d68-4028-b16a-cb7e20b92076
---

User pain (2026-07-16): RE-derived names/formatting make the codebase and LLM-gen code
hard to read; fears "many refactoring sessions before shipping v1 leashed co-op".

POLICY (agreed):
- RE names stay UNTIL post-v1 — they are stable identifiers into upstream fallout2-ce/
  sfall/community RE knowledge; renaming mid-rewrite burns greppability while recon
  still depends on it.
- Quarantine line: NEW seams/files always get real names (server_control, server_anim,
  combatRoundBegin...). LLM-gen code in new files must read modern; reviews may carry a
  readability lens.
- **GLOSSARY.md (repo root, @d179bd5)** = additive-only decoder ring; every session that
  decodes an ugly name the hard way APPENDS a line. It doubles as the RENAME MAP for the
  banked post-v1 rename pass.
- The big rename is LOW-RISK here by construction: renames are behavior-invariant and the
  golden gates compare behavior byte-for-byte — schedule it as bulk mechanical work after
  v1 leash ships, never before (buys no features, invalidates in-flight docs/recon).
- Related banked: folder restructure server/core/client (see MEMORY.md IDEAS line) rides
  the same post-v1 window.

**Why:** converts recon knowledge that currently evaporates into a compounding asset, and
sequences the readability tax where it's cheap instead of where it stalls the roadmap.
**How to apply:** when writing recon/build briefs, tell agents to append decoded names to
GLOSSARY.md; when user raises readability again, point at this policy + the glossary's
growth instead of starting an early rename.
