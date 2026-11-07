/**
 * Soft generative background music, synthesised with the Web Audio API (no audio files).
 * A slow four-chord pad (Cmaj7, Am9, Fmaj7, G6) through a low-pass filter and a generated
 * reverb, plus sparse pentatonic plucks. Browsers only allow audio after a user gesture, so
 * start() is called from the first click or key press.
 */

const CHORDS = [
  [48, 55, 59, 64], // Cmaj7
  [45, 52, 55, 59, 62], // Am9 (no root doubling)
  [41, 48, 52, 57], // Fmaj7
  [43, 50, 55, 57], // G6
];
const PLUCK = [72, 74, 76, 79, 81, 84]; // C major pentatonic, upper octave
const CHORD_SECONDS = 9;
const LEVEL = 0.055;
const DUCKED = 0.014;

const hz = (m: number) => 440 * Math.pow(2, (m - 69) / 12);

export class Ambient {
  private ctx: AudioContext | null = null;
  private master: GainNode | null = null;
  private bus: AudioNode | null = null;
  private timer: number | null = null;
  private nextChord = 0;
  private nextPluck = 0;
  private chordIx = 0;
  private ducked = false;
  running = false;

  private build() {
    const AC = window.AudioContext ?? (window as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext;
    const ctx = new AC();
    const master = ctx.createGain();
    master.gain.value = 0;
    master.connect(ctx.destination);

    // reverb: exponentially decaying noise impulse, about 4 s
    const len = Math.floor(ctx.sampleRate * 4);
    const ir = ctx.createBuffer(2, len, ctx.sampleRate);
    for (let ch = 0; ch < 2; ch++) {
      const d = ir.getChannelData(ch);
      for (let i = 0; i < len; i++) d[i] = (Math.random() * 2 - 1) * Math.pow(1 - i / len, 3);
    }
    const verb = ctx.createConvolver();
    verb.buffer = ir;
    const wet = ctx.createGain();
    wet.gain.value = 0.55;
    const dry = ctx.createGain();
    dry.gain.value = 0.6;
    const lp = ctx.createBiquadFilter();
    lp.type = "lowpass";
    lp.frequency.value = 1100;
    lp.Q.value = 0.3;
    lp.connect(dry).connect(master);
    lp.connect(verb).connect(wet).connect(master);

    this.ctx = ctx;
    this.master = master;
    this.bus = lp;
  }

  private pad(t: number, notes: number[]) {
    const ctx = this.ctx!;
    const dur = CHORD_SECONDS + 4;
    for (const m of notes) {
      for (const [type, detune, level] of [
        ["sine", -4, 0.16],
        ["triangle", 5, 0.07],
      ] as const) {
        const o = ctx.createOscillator();
        o.type = type;
        o.frequency.value = hz(m);
        o.detune.value = detune;
        const g = ctx.createGain();
        g.gain.setValueAtTime(0, t);
        g.gain.linearRampToValueAtTime(level / notes.length, t + 3.2);
        g.gain.setValueAtTime(level / notes.length, t + dur - 4.5);
        g.gain.linearRampToValueAtTime(0, t + dur);
        o.connect(g).connect(this.bus!);
        o.start(t);
        o.stop(t + dur + 0.1);
      }
    }
  }

  private pluck(t: number) {
    const ctx = this.ctx!;
    const m = PLUCK[Math.floor(Math.random() * PLUCK.length)];
    const o = ctx.createOscillator();
    o.type = "sine";
    o.frequency.value = hz(m);
    const g = ctx.createGain();
    g.gain.setValueAtTime(0, t);
    g.gain.linearRampToValueAtTime(0.05, t + 0.02);
    g.gain.exponentialRampToValueAtTime(0.0001, t + 3.2);
    o.connect(g).connect(this.bus!);
    o.start(t);
    o.stop(t + 3.3);
  }

  private schedule = () => {
    const ctx = this.ctx;
    if (!ctx) return;
    const ahead = ctx.currentTime + 1.5;
    while (this.nextChord < ahead) {
      this.pad(this.nextChord, CHORDS[this.chordIx % CHORDS.length]);
      this.chordIx++;
      this.nextChord += CHORD_SECONDS;
    }
    while (this.nextPluck < ahead) {
      if (Math.random() < 0.7) this.pluck(this.nextPluck);
      this.nextPluck += 2.2 + Math.random() * 3.2;
    }
  };

  async start() {
    if (this.running) return;
    if (!this.ctx) this.build();
    const ctx = this.ctx!;
    await ctx.resume();
    this.running = true;
    const now = ctx.currentTime;
    this.nextChord = Math.max(this.nextChord, now + 0.1);
    this.nextPluck = Math.max(this.nextPluck, now + 2);
    this.master!.gain.cancelScheduledValues(now);
    this.master!.gain.setTargetAtTime(this.ducked ? DUCKED : LEVEL, now, 1.2);
    this.timer = window.setInterval(this.schedule, 400);
    this.schedule();
  }

  stop() {
    if (!this.running || !this.ctx) return;
    this.running = false;
    const ctx = this.ctx;
    this.master!.gain.setTargetAtTime(0, ctx.currentTime, 0.4);
    if (this.timer !== null) window.clearInterval(this.timer);
    this.timer = null;
    window.setTimeout(() => {
      if (!this.running) void ctx.suspend();
    }, 2500);
  }

  /** Lower the music while the film narration speaks. */
  duck(on: boolean) {
    this.ducked = on;
    if (this.running && this.ctx) this.master!.gain.setTargetAtTime(on ? DUCKED : LEVEL, this.ctx.currentTime, 0.3);
  }
}
