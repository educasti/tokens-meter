# Open PR, merge, delete the remote branch

Repo: /Users/educasti/Projects/Personal/tokens-meter. Branch `feat/opencode-screens` is pushed to `origin` (https://github.com/educasti/tokens-meter), commits `5b87b01` (feature) and `3090bb5` (docs) on top of `main`.

**This repo is a fork of HermannBjorgvin/Clawdmeter. The PR MUST target the fork itself — `--repo educasti/tokens-meter --base main` — never the upstream repo.** Verify with `gh pr view` after creating it that the base repo is educasti/tokens-meter.

1. `gh auth status` (stop and report if not authenticated).
2. Create the PR: `gh pr create --repo educasti/tokens-meter --base main --head feat/opencode-screens`, title `Add OpenCode splash and usage screens`. Body (markdown): a short summary from the two commit messages (`git log main..feat/opencode-screens`), a "Verification" list (7 board envs + 3 sim envs build, 165 daemon tests pass, sim screenshots match the approved prototype, hardware test on AMOLED-2.16 — see `design/opencode-screen/research/hw1-test-216.md`), and end the body with exactly:
   `🤖 Generated with [Claude Code](https://claude.com/claude-code)`
3. Merge it with a merge commit and delete the remote branch: `gh pr merge <number> --repo educasti/tokens-meter --merge --delete-branch`.
4. Locally: `git switch main && git pull --ff-only origin main`, then `git branch -d feat/opencode-screens` (only if fully merged) and `git fetch --prune`. Do not touch `.vscode/` or untracked files.
5. Report: PR URL, merge commit hash, confirmation that `origin/feat/opencode-screens` no longer exists (`git ls-remote --heads origin feat/opencode-screens` is empty), and `git log --oneline -3` on main.

Do not force-push. Reply `DONE` + the report.
