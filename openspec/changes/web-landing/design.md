# Design: web-landing

## Stack
- Vite + React 18 + TypeScript, Tailwind CSS v4 (`@tailwindcss/vite`). No router library: a 30-line hash router (`#/install`), which also works on any static host including GitHub Pages.
- No animation, icon, or UI library. Animations are SVG elements whose attributes are computed from a time value each frame (`requestAnimationFrame`). Target: under 80 kB gzipped JavaScript.
- Fonts (Google Fonts): IBM Plex Mono for body and code, Space Grotesk for headings, Caveat for hand-written annotations only. No Inter/Roboto, no italic serif hero.

## Visual language
- Only #000 and #fff. Tones come from line weight, dashes, hatching, and dot patterns, never grey fills or opacity.
- Paper: white page, a faint dot grid made of 1 px black dots (body background), a margin rule on wide screens.
- Ink: strokes animate in with `stroke-dashoffset`; a deterministic "wobble" turns straight lines into hand-drawn ones (seeded, so it does not jitter between frames).
- Boxes have square corners drawn as slightly irregular ink outlines. Primary buttons are solid black with a hard 3 px offset shadow; they press down on click. Active nav item gets a hand-drawn underline scribble.
- Annotations in Caveat point at things with hand-drawn arrows, like margin notes.
- Forbidden (from the brief): gradients, glows, neon, eyebrow labels in caps, icon-card grids, uniform rounded corners, fake testimonials, placeholder images, vague copy, em dashes.

## Pages
1. Home: plain headline and one-sentence explanation; hero animation (sentence → tokens → embedding column → arrow in a rotating 3D space); a short "how search works" animation (query arrow, nearest neighbours, cosine angle); a ledger of verifiable facts (97 parity tests, 4 types, 6 operators, 18 MB image); a 10-line code sample; links to install and architecture.
2. Install: README quick start as steps, with tabs for Docker / Linux binary, copy buttons on code, an environment-variable table, SDK install, first search.
3. Architecture: the film. A stage (SVG, 16:9 on desktop, 4:5 on phones) plays eight scenes: the parts, a write, signing and the chain, sync back, search, expiry, reorg rollback, on-chain layout and the math. Subtitles scroll in a box below the stage, time-synced. Controls: play/pause, previous/next chapter, scrubbable pencil-line progress bar with chapter ticks. Below: chapter list and the full transcript as text.
4. Team: one entry. Dikshit Rishi Jain, NITK Surathkal, dikshitrjain@gmail.com, built for ETHGlobal Mumbai 2026. No avatar.

## Timeline engine
`useTimeline(totalMs)` returns `{t, playing, play, pause, seek}`. Scenes declare `[start, end]`; a scene renders from local progress `p = clamp((t-start)/(end-start))`. Helpers: `ease`, `seg(p, a, b)` (sub-range progress), `lerp`. Everything is a pure function of `t`, so scrubbing and reduced motion (render the final frame) are free.

## Responsiveness
- Mobile-first Tailwind. Breakpoints: single column under 768 px; two-column hero and margin notes from `md`.
- SVG stages use `viewBox` and scale to width; the film switches to a taller viewBox layout on narrow screens so labels stay legible (min 12 px rendered text).
- Nav collapses to a single row of short links; no hamburger needed for four pages.
- Code blocks scroll horizontally inside their box; buttons never wrap their label (`whitespace-nowrap`).
- `prefers-reduced-motion`: animations show their final frame; the film starts paused.
