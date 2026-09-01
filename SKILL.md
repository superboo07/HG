---
name: handoff
description: Generate a "handoff prompt" that lets a brand-new agent (with zero memory of this conversation) pick up the current work seamlessly. Trigger this whenever the user types "/handoff", asks to "hand off" or "hand this off", asks to summarize the conversation "for a new agent/chat/session", says they're "running out of context" or "hitting the context limit" and want to continue elsewhere, asks to "continue this in a new chat", or otherwise wants a self-contained briefing document that transfers the current task to a fresh assistant instance. Always use this skill for these requests instead of writing an ad-hoc summary — it defines the required structure and level of detail a handoff prompt needs to actually work.
---

# Handoff

Generate a single, self-contained prompt that a brand-new agent — with no access to this conversation, no memory, and no shared context — can be given as their *first message* and immediately continue the work correctly.

## Core principle

Assume the new agent knows **nothing** except what's in the handoff prompt. It has not seen this conversation, cannot see the user's screen, and cannot infer intent from tone. Anything not written down is lost. Err on the side of including detail that seems obvious to you right now — it won't be obvious to the new agent.

At the same time, do not pad the prompt with irrelevant conversational history, pleasantries, or resolved side-quests. The goal is a dense, high-signal briefing, not a transcript.

## When to trigger

- The user types `/handoff` on its own or with extra instructions (e.g. `/handoff focus on the API work`)
- Explicit asks: "hand this off", "generate a handoff prompt", "help me continue this in a new chat/session"
- Context-limit signals: "I'm running low on context", "this chat is getting too long", "let's continue somewhere else"
- Any request for a summary explicitly meant to brief a *new agent/assistant*, as opposed to a summary for the user themselves

If the user just wants a normal recap for their own reading ("summarize what we did"), that's not this skill — only trigger when the summary's audience is a future agent/assistant.

## What to gather before writing

Scan the full conversation (and any files touched, commands run, or artifacts created) for:

1. **The overall goal** — what the user is ultimately trying to accomplish, in their own terms
2. **Current state** — what has actually been done so far, concretely (files created/modified with paths, code written, decisions made, research findings, artifacts produced)
3. **Key decisions and constraints** — choices made along the way and *why* (especially ones that weren't obvious, or where an alternative was explicitly rejected), plus any hard constraints the user stated (deadlines, tech stack, style preferences, things to avoid)
4. **Open threads** — what's unfinished, what the immediate next step is, and any known blockers or unresolved questions
5. **Pitfalls already discovered** — dead ends tried, bugs found and fixed, things that looked right but weren't — so the new agent doesn't repeat the same mistakes
6. **Relevant artifacts/files** — exact paths, filenames, or links the new agent will need to open or reference

If something is ambiguous or you can't tell whether a thread is resolved, note it as an open question in the handoff rather than guessing silently.

## Output format

Produce the handoff as a single markdown block the user can copy-paste directly as the first message to a new agent. Use this structure, omitting sections that are genuinely empty (don't force padding into an empty section):

**CRITICAL — fence length.** A handoff almost always ends up containing fenced code blocks of its own (shell recipes, config snippets, file excerpts). A three-backtick outer wrapper is closed by the **first** inner three-backtick fence, which silently truncates the handoff mid-document and breaks copy-paste. So:

- Open and close the outer wrapper with **four backticks** (````` ```` `````) by default — as shown below.
- If the handoff's own content contains a four-backtick fence, use five, and so on. **The outer wrapper must always be strictly longer than the longest fence inside it.**
- Inner code blocks keep their normal three backticks. Do NOT escape them, indent them, or downgrade them to inline code — that damages the content the new agent needs.
- Before delivering, re-read what you emitted and confirm the closing wrapper is the last line, and that nothing after the first inner code block leaked outside the wrapper.

````markdown
# Handoff: <short task title>

## Goal
<1-3 sentences: what we're trying to accomplish and for whom/why, if relevant>

## Current state
<Concrete description of progress. Bullet points. Include file paths, what's working, what's been built.>

## Key decisions
<Bullets: decision -> brief reason. Only include non-obvious or consequential ones.>

## Constraints & preferences
<Anything the user explicitly stated: deadlines, style, tools, things to avoid>

## Next steps
<Ordered list of what should happen next, starting with the immediate next action>

## Watch out for
<Dead ends, known bugs, gotchas already discovered — so they aren't repeated>

## Relevant files/links
<Exact paths or URLs the new agent needs>
````

Keep prose tight — bullets over paragraphs wherever possible. The whole thing should be readable in under a minute but leave nothing essential out.

## Delivery

- Present the handoff prompt inline in a markdown code block (not as a downloadable file) by default, since the user will copy it directly into a new chat. Apply the fence-length rule above — a truncated wrapper is the single most common way this deliverable fails.
- After presenting it, briefly tell the user it's ready to paste into a new conversation. Don't add lengthy commentary — the handoff prompt itself is the deliverable.
- If the user gave a focus area (e.g. `/handoff just the frontend work`), scope the handoff to that, but still include enough of the overall goal for context to make sense standing alone.
