# Proposal: web-landing

## Why
Arkivector is entered at ETHGlobal Mumbai 2026. Judges and developers need one place that says what it is, shows how it works, and gets the daemon running. The README does this in text. A small site can show the two ideas that are hard to get from text: what an embedding is, and how a write travels to the chain and back into search.

## What changes
- New `web/` folder: a static React + TypeScript + Tailwind site built with Vite. No backend.
- Pages: home, install, architecture (diagram + narrated film), build artifacts, what's next, team, and a 404 page.
- A pen-and-paper visual language in pure black (#000) and white (#fff): ruled and dotted paper, ink strokes that draw themselves, hand-lettered annotations.
- SVG animations driven by a small timeline engine (no animation library):
  - text turning into a vector and landing in a rotating 3D space;
  - nearest-neighbour search with a cosine angle;
  - an architecture "film": scenes with silent playback and subtitles scrolling below, scrubbable, with a readable transcript.
- Install page content mirrors the README quick start (Docker, Linux binary, wallet, environment variables, SDKs, first search).

## Capabilities
- `landing-site`: static marketing and documentation site for Arkivector.

## Out of scope
Backend, analytics, a hosted demo against a live daemon, code documentation, integration tests, deployment workflow. Audio is synthesised in the browser (Web Audio, speech synthesis); no audio files ship.

## Impact
New code under `web/` only. No change to the daemon, the SDKs, or the contract.
