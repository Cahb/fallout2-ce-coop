---
name: memory-doc-hygiene
description: "Memory/MD files hold ONLY quirks, known issues, and the forward plan. No changelog, no \"we resolved it\", no completed-work narration. Completed → delete it."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 4169d5ed-c9b8-434c-9df1-571a8d0011ba
---

Keep memory files AND repo .md docs lean and FORWARD-LOOKING. Only three things earn a
place: **quirks/gotchas** (non-obvious engine behavior, traps), **known issues/open
risks**, and **the real plan** (what's next + banked-future work).

Do NOT keep: "we did X / DONE @hash / eyeball-verified", session dates, blow-by-blow of
past actions, "review CLEAN", or any narration of completed work. Git history is the
changelog — memory is not. **When something is completed, DELETE it from the md/memory**
rather than marking it done. A commit hash for finished work belongs in git, not here.

**Why:** the user called this out directly (2026-07-16) — the "we resolved it" / previous-
actions bloat is useless and buries the signal (quirks + plan). It also grows unbounded.

**How to apply:** when a slice/task finishes, prune its status narrative out of the doc/
memory in the SAME change; carry forward only any quirk it surfaced (as a quirk entry) and
any newly-banked future work. A durable quirk gets its own small memory (e.g.
[[frame-index-render-gotcha]]), not a paragraph in a changelog. Slim description
frontmatter too — it's a hook, not a history.
