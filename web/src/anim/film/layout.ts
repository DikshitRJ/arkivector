/** Stage geometry for the architecture film: a wide layout and a tall one for phones. */

export type R = { x: number; y: number; w: number; h: number };

export type Layout = {
  vw: number;
  vh: number;
  fs: number; // font scale
  app: R;
  daemon: R;
  http: R;
  writer: R;
  planner: R;
  filter: R;
  store: R;
  syncer: R;
  chain: R;
  /** where the visible block slot i sits (0 = oldest visible) */
  block: (i: number) => R;
  slots: number;
  vertical: boolean; // chain drawn top-to-bottom (desktop) or left-to-right (phone)
};

export const WIDE: Layout = {
  vw: 960,
  vh: 540,
  fs: 1,
  app: { x: 22, y: 196, w: 176, h: 112 },
  daemon: { x: 246, y: 30, w: 478, h: 480 },
  http: { x: 266, y: 78, w: 210, h: 54 },
  writer: { x: 494, y: 78, w: 210, h: 54 },
  planner: { x: 266, y: 150, w: 210, h: 132 },
  filter: { x: 494, y: 150, w: 210, h: 132 },
  store: { x: 266, y: 300, w: 438, h: 112 },
  syncer: { x: 266, y: 430, w: 438, h: 62 },
  chain: { x: 768, y: 30, w: 170, h: 480 },
  block: (i) => ({ x: 790, y: 86 + i * 70, w: 126, h: 52 }),
  slots: 6,
  vertical: true,
};

export const TALL: Layout = {
  vw: 540,
  vh: 860,
  fs: 1.32,
  app: { x: 18, y: 14, w: 504, h: 82 },
  daemon: { x: 18, y: 124, w: 504, h: 516 },
  http: { x: 32, y: 176, w: 230, h: 60 },
  writer: { x: 278, y: 176, w: 230, h: 60 },
  planner: { x: 32, y: 250, w: 230, h: 148 },
  filter: { x: 278, y: 250, w: 230, h: 148 },
  store: { x: 32, y: 412, w: 476, h: 128 },
  syncer: { x: 32, y: 554, w: 476, h: 72 },
  chain: { x: 18, y: 662, w: 504, h: 184 },
  block: (i) => ({ x: 34 + i * 98, y: 728, w: 86, h: 92 }),
  slots: 5,
  vertical: false,
};

export const cx = (r: R) => r.x + r.w / 2;
export const cy = (r: R) => r.y + r.h / 2;
export const mid = (r: R): [number, number] => [cx(r), cy(r)];
export const left = (r: R): [number, number] => [r.x, cy(r)];
export const right = (r: R): [number, number] => [r.x + r.w, cy(r)];
export const top = (r: R): [number, number] => [cx(r), r.y];
export const bottom = (r: R): [number, number] => [cx(r), r.y + r.h];
