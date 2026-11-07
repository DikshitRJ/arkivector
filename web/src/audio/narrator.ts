/**
 * Film narration with the browser's built-in speech synthesis (no audio files).
 * Picks a natural-sounding English voice when one is installed.
 */

const PREFERRED = [
  /Google UK English Female/i,
  /Google US English/i,
  /Microsoft (Aria|Jenny|Guy|Libby|Sonia).*Online/i,
  /Samantha|Daniel|Karen|Moira|Serena/i,
  /English.*(Natural|Neural)/i,
];

export const speechSupported = () => typeof window !== "undefined" && "speechSynthesis" in window;

function pickVoice(): SpeechSynthesisVoice | null {
  const voices = window.speechSynthesis.getVoices().filter((v) => v.lang.toLowerCase().startsWith("en"));
  for (const re of PREFERRED) {
    const v = voices.find((x) => re.test(x.name));
    if (v) return v;
  }
  return voices.find((v) => v.default) ?? voices[0] ?? null;
}

if (speechSupported()) {
  // voices load asynchronously in Chrome; touching the list starts the load
  window.speechSynthesis.getVoices();
}

/**
 * Speak `text`. Calls `done` when speech ends, fails, or after a safety timeout (so the film
 * never waits forever on a browser without voices). Returns a cancel function.
 */
export function speak(text: string, done: () => void): () => void {
  if (!speechSupported()) {
    done();
    return () => {};
  }
  const synth = window.speechSynthesis;
  synth.cancel();
  const u = new SpeechSynthesisUtterance(text);
  const v = pickVoice();
  if (v) u.voice = v;
  u.lang = v?.lang ?? "en-GB";
  u.rate = 1.04;
  u.pitch = 1;
  let finished = false;
  const finish = () => {
    if (finished) return;
    finished = true;
    window.clearTimeout(guard);
    done();
  };
  // ~170 words per minute plus slack
  const guard = window.setTimeout(finish, 2500 + text.split(/\s+/).length * 480);
  u.onend = finish;
  u.onerror = finish;
  synth.speak(u);
  return () => {
    finished = true;
    window.clearTimeout(guard);
    synth.cancel();
  };
}

export function stopSpeaking() {
  if (speechSupported()) window.speechSynthesis.cancel();
}
