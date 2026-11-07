import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { Ambient } from "./ambient";

type Sound = {
  music: boolean;
  toggleMusic: () => void;
  duck: (on: boolean) => void;
};

const Ctx = createContext<Sound>({ music: false, toggleMusic: () => {}, duck: () => {} });
const KEY = "arkivector-music";

function readPref(): boolean {
  try {
    return localStorage.getItem(KEY) !== "off";
  } catch {
    return true;
  }
}

/**
 * Owns the background music for the whole app, so it keeps playing across page changes.
 * Music is on by default but only starts after the first user gesture (browser autoplay rules).
 */
export function SoundProvider({ children }: { children: ReactNode }) {
  const amb = useRef<Ambient | null>(null);
  const [music, setMusic] = useState(readPref);
  const gestured = useRef(false);

  const get = () => (amb.current ??= new Ambient());

  useEffect(() => {
    const first = () => {
      gestured.current = true;
      if (music) void get().start();
    };
    window.addEventListener("pointerdown", first, { once: true });
    window.addEventListener("keydown", first, { once: true });
    return () => {
      window.removeEventListener("pointerdown", first);
      window.removeEventListener("keydown", first);
    };
  }, [music]);

  const toggleMusic = useCallback(() => {
    setMusic((m) => {
      const next = !m;
      try {
        localStorage.setItem(KEY, next ? "on" : "off");
      } catch {
        /* storage blocked: preference lasts for this visit only */
      }
      if (next) void get().start();
      else get().stop();
      return next;
    });
  }, []);

  const duck = useCallback((on: boolean) => amb.current?.duck(on), []);

  const value = useMemo(() => ({ music, toggleMusic, duck }), [music, toggleMusic, duck]);
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export const useSound = () => useContext(Ctx);
