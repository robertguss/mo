---
name: notion-sync
description: Bring the Mo v2 Notion hub up to date with live GitHub state. Use after a PR opens, merges or changes CI state, after Robert makes a decision, or when asked to update Notion or the hub.
---

# Notion sync

The Notion hub is Robert's single view of Mo. GitHub is the source of truth; the hub's Status page is its summary, and it **drifts** every time a PR moves.

## Where things are

| What | Notion ID |
| --- | --- |
| Hub page "Mo v2" (Dev Hub > Projects): only links to sub-pages, never status text | `3f5700320c4581e0aa31cbdff9f114ff` |
| Status page under the hub, the page this skill keeps current; its two task tables are live Mo Tasks views | `3fd700320c45810ca58ddf73fb6fc8c2` |
| Changelog page under the hub: every merged PR, newest first, grouped by day | `3fd700320c4581d083f7ca20eb6e0903` |
| Archive page under the hub: old hub history and retired pages | `3fd700320c4581998298fbb834ad7377` |
| Mo Tasks DB (Task, Status, PR, Blocked by, Stream; "Pull requests" and "PR state" are filled by the GitHub sync) | `collection://1b61c271-224a-4e05-998c-b0bc93e325f9` |
| Decision Log DB (Number, Decision "Dnnn · title", Date, Status, Robert's words, Supersedes or refines) | `collection://bb053cc4-7cf0-49da-bd28-2205d3caf386` |
| Phase 2 spec page, which holds the checklist the Status page counts | `3f6700320c4581c4b7c4dafa8f459b52` |

The Notion connector can be signed in to Robert's work workspace instead. Call `notion-get-self` first; the workspace must be "Robert's Notion". If it isn't, stop and ask Robert to reconnect his personal account.

## 1. Read live state

From GitHub: main's head SHA, and for every MO PR its state (open, draft, merged, closed), head SHA, mergeable state and latest `check` result, plus the merge commit SHA and time of each merged PR. Read the Status page, the Changelog, the Mo Tasks rows for those PRs, and the latest Decision Log rows fresh. A summary from memory or an earlier session is a lead to check, never a fact to copy.

Done when you hold the live row for every PR the Status page or Mo Tasks mentions.

## 2. Find the drift

Compare each of these spots with the live state. These go stale first:

- on the Status page under "Main": the "As of" line with main's SHA, "What main can do", "What main can't do yet", the samples count, "Open PRs", and "Next up";
- the Phase 2 paragraph's checklist count at the top of the Status page;
- the Changelog: every merged PR is listed under its day;
- "Still to do for Phase 2" and "Waiting for Robert";
- "Recent decisions" against the Decision Log;
- each task's Status and PR in Mo Tasks: an open PR is "In review", a merged one "Done";
- any note saying CI is pending, red, or conflicting, and any PR described as open that has since merged.

## 3. Fix

Rewrite each clearly stale fact to match GitHub, with the PR number, short SHA, and date. Replace stale text in place rather than adding a newer bullet above it. A merged PR goes on the Changelog, never on the Status page: Status says where main is now, not how it got there. Don't add status text to the hub page itself; it only lists sub-pages. Don't write the tasks in progress as prose; the Status page's Mo Tasks views show them. Write the "As of" time as now, in UTC.

A decision Robert made becomes a Decision Log row with the next number, today's date, and his words quoted exactly, then a line in "Recent decisions" on the Status page, which keeps the latest eight. Write the hub in Robert's plain style: full sentences, no internal session or tool names.

A fact you cannot settle from GitHub (a review Robert may have given offline, a decision implied but never stated) stays as it is; list it for Robert.

Done when every spot in step 2 matches the live state, or is on the list for Robert.

## 4. Report

Tell Robert in two or three lines what changed on the Status page and Changelog, then the open list.
