import { useEffect, useRef } from "react";

/*
 * Background vector field: a grid of Arkiv-blue arrows, each about 1 cm long (38 CSS px),
 * that always point at the cursor (or the last touch). Arrows near the target are more opaque,
 * so the field fades in around the pointer. Drawn on one fixed canvas; redraws only when the
 * target moves. With no pointer (or reduced motion) the target drifts slowly / stays centred.
 */

const SPACING = 58; // px between arrow centres
const LEN = 38; // ~1 cm at 96 dpi
const RGB = "24, 30, 169"; // Arkiv blue #181EA9
const A_FAR = 0.045;
const A_NEAR = 0.14;

export function VectorField() {
  const ref = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    const reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    let w = 0;
    let h = 0;
    let dpr = 1;
    let tx = window.innerWidth / 2;
    let ty = window.innerHeight / 2;
    let hasPointer = false;
    let raf = 0;
    let dirty = true;
    let idle = 0;

    const resize = () => {
      dpr = Math.min(window.devicePixelRatio || 1, 2);
      w = window.innerWidth;
      h = window.innerHeight;
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
      canvas.style.width = `${w}px`;
      canvas.style.height = `${h}px`;
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      if (!hasPointer) {
        tx = w / 2;
        ty = h / 2;
      }
      dirty = true;
    };

    const draw = () => {
      ctx.clearRect(0, 0, w, h);
      ctx.lineCap = "round";
      ctx.lineJoin = "round";
      ctx.lineWidth = 1.4;
      const reach = Math.hypot(w, h) * 0.55;
      const ox = ((w % SPACING) + SPACING) / 2;
      const oy = ((h % SPACING) + SPACING) / 2;
      for (let y = oy - SPACING / 2; y < h + SPACING; y += SPACING) {
        for (let x = ox - SPACING / 2; x < w + SPACING; x += SPACING) {
          const dx = tx - x;
          const dy = ty - y;
          const d = Math.hypot(dx, dy) || 1;
          const ux = dx / d;
          const uy = dy / d;
          // shrink arrows sitting right under the pointer so they don't pile up on it
          const len = LEN * Math.min(1, 0.35 + d / 90);
          const k = Math.max(0, 1 - d / reach);
          const a = A_FAR + (A_NEAR - A_FAR) * k * k;
          const x0 = x - ux * len * 0.5;
          const y0 = y - uy * len * 0.5;
          const x1 = x + ux * len * 0.5;
          const y1 = y + uy * len * 0.5;
          const hs = 7;
          ctx.strokeStyle = `rgba(${RGB}, ${a.toFixed(3)})`;
          ctx.beginPath();
          ctx.moveTo(x0, y0);
          ctx.lineTo(x1, y1);
          // head: two short strokes at +-28 degrees
          const c = Math.cos(0.5);
          const s = Math.sin(0.5);
          ctx.moveTo(x1 - hs * (ux * c - uy * s), y1 - hs * (uy * c + ux * s));
          ctx.lineTo(x1, y1);
          ctx.lineTo(x1 - hs * (ux * c + uy * s), y1 - hs * (uy * c - ux * s));
          ctx.stroke();
        }
      }
    };

    const loop = (now: number) => {
      if (!hasPointer && !reduced) {
        // drift slowly around the centre until the visitor moves the pointer
        idle = now / 1000;
        tx = w / 2 + Math.cos(idle * 0.35) * w * 0.28;
        ty = h / 2 + Math.sin(idle * 0.5) * h * 0.22;
        dirty = true;
      }
      if (dirty) {
        draw();
        dirty = false;
      }
      raf = requestAnimationFrame(loop);
    };

    const onMove = (e: PointerEvent) => {
      hasPointer = true;
      tx = e.clientX;
      ty = e.clientY;
      dirty = true;
    };
    const onLeave = () => {
      hasPointer = false;
    };

    resize();
    window.addEventListener("resize", resize);
    window.addEventListener("pointermove", onMove, { passive: true });
    window.addEventListener("pointerdown", onMove, { passive: true });
    document.documentElement.addEventListener("pointerleave", onLeave);
    raf = requestAnimationFrame(loop);
    return () => {
      cancelAnimationFrame(raf);
      window.removeEventListener("resize", resize);
      window.removeEventListener("pointermove", onMove);
      window.removeEventListener("pointerdown", onMove);
      document.documentElement.removeEventListener("pointerleave", onLeave);
    };
  }, []);

  return <canvas ref={ref} aria-hidden className="pointer-events-none fixed inset-0 -z-10" />;
}
