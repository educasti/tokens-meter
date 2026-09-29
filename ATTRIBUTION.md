# Attribution

This repository is a personal fork of **[Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)** by [Hermann Bjorgvin](https://github.com/HermannBjorgvin).

All original code, firmware, tooling, documentation and commit history in this
repository originate from that project. The upstream author is
[HermannBjorgvin](https://github.com/HermannBjorgvin); please credit them if you
reuse any of this work.

- Upstream: <https://github.com/HermannBjorgvin/Clawdmeter>
- Fork owner: @educasti
- Based on upstream commit `85a0693`

Changes made in this fork are my own and are not endorsed by the upstream
author.

## Third-party assets

- **OpenCode logo and wordmark** — the mark and wordmark on the OpenCode screens
  are redrawn as pixel grids from the official SVGs in
  [anomalyco/opencode](https://github.com/anomalyco/opencode), which is MIT
  licensed ("Copyright (c) 2025 opencode"). The MIT text grants use of the files
  but says nothing about trademark rights, and that repo carries no separate
  trademark policy. **"OpenCode" is a trademark of its owners.** This fork is
  unaffiliated; nothing here implies endorsement by, or affiliation with, the
  OpenCode project. The art is unaltered apart from the brand palette recorded
  in `design/opencode-screen/SPEC.md` §3. Confirm the terms on
  <https://opencode.ai/brand> before redistributing.
- **IBM Plex Mono** — the type family of the OpenCode screens
  (`firmware/src/font_plex_*.c`), from [IBM/plex](https://github.com/IBM/plex)
  under the **SIL Open Font License 1.1**. The license text is vendored next to
  the TTFs it covers, at `assets/fonts/OFL.txt`.
- **Lucide** — the bluetooth and battery UI glyphs, MIT
  ([lucide.dev](https://lucide.dev)).

## Licensing status

**This fork has no license of its own, and neither does upstream.**

The upstream project ships no `LICENSE` file, and its README explicitly states
that the author deliberately did not apply a copyleft license because the
repository bundles assets they do not have the right to relicense:

- Anthropic brand fonts (Tiempos Text, Styrene B), used under a license held by
  Anthropic that upstream is using without permission.
- Anthropic's copyrighted Clawd mascot sprites (`research/clawd-official/`,
  `assets/`, `firmware/src/splash_animations.h`).

With no license grant, default copyright applies: all rights reserved. Redistribution
of this code, and especially of those proprietary font and mascot assets, is not
authorized by the author.

I am publishing this fork for personal use and reference. If you intend to
redistribute it or build on it publicly, resolve the licensing situation with
the upstream author first, or strip the proprietary assets — the daemon,
`tools/`, and the per-board firmware code in `firmware/src/boards/` are the
portions that are not affected by the font/mascot issue.
