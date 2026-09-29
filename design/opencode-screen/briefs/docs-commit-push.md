# Docs touch-up, commit and push

Repo: /Users/educasti/Projects/Personal/tokens-meter, branch `feat/opencode-screens` (last commit `5b87b01` "Add OpenCode splash and usage screens"). The OpenCode screens were verified on a real Waveshare AMOLED-2.16; the report is `design/opencode-screen/research/hw1-test-216.md` (read it).

## 1. Docs (edit only these files; keep each file's style, English, concise)
- `README.md`, "Prerequisites": the macOS line says `python3` — change it to **Python 3.10+** (macOS's system python3 is 3.9 and the daemon uses `X | None` syntax; `install-mac.sh` already picks Homebrew's Python ≥ 3.10 automatically — say so, and suggest `brew install python` if none is found).
- `README.md`, "OpenCode screens" → "Enabling them": after turning on `opencode = on`, the running daemon must be **restarted** to load the new collector module (`launchctl kickstart -k gui/$(id -u)/com.user.claude-usage-daemon`). If the LaunchAgent was installed from another checkout, re-run `./install-mac.sh` from this one so the plist points at this repo.
- `CLAUDE.md`: in the "OpenCode collector (macOS daemon only)" paragraph add one sentence: verified on a real AMOLED-2.16 (see `design/opencode-screen/research/hw1-test-216.md`); the daemon needs Python ≥ 3.10. Nothing else.
- `design/opencode-screen/SPEC.md`: change the status line at the top from "propuesta… para aprobar" to say it is **implemented** (commit `5b87b01`, branch `feat/opencode-screens`) and verified on the AMOLED-2.16. Keep it in Spanish.

## 2. Commit
- `git add` only the files above (do NOT add `.vscode/`).
- Commit message: `docs: OpenCode screens — Python 3.10+, daemon restart note, hw-verified status` and end the message body with the line:
  `Co-Authored-By: Claude Haiku 4.5 <noreply@anthropic.com>`

## 3. Push
- `git push -u origin feat/opencode-screens`. Do not force-push, do not push other branches, do not open a PR.
- Report the push output (branch URL / PR-suggestion line).

Reply `DONE` + commit hash + push result.
