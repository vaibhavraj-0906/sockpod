// A pretend pod for trying the app without hardware. It sends the same live and
// session JSON as the real firmware, following a scripted one-minute workout.

// One entry per second of each minute: [activity, cadence, intensity (ENMO g)]
const SCRIPT = [
  ...Array(5).fill(['rest', 0, 0.02]),
  ...Array(15).fill(['walk', 112, 0.18]),
  ...Array(15).fill(['jog', 166, 0.42]),
  ...Array(7).fill(['sprint', 196, 0.95]),
  ...Array(8).fill(['jumps', 0, 0.55]),
  ...Array(10).fill(['rest', 0, 0.03]),
];

const jitter = (x, spread) => x + (Math.random() * 2 - 1) * spread;
const round1 = (x) => Math.round(x * 10) / 10;

export class DemoTransport {
  source = 'demo';

  constructor({ onLive, onStatus, firstId = 0 }) {
    this.onLive = onLive;
    this.onStatus = onStatus;
    this.lastId = firstId;
    this.summary = { id: 0 };
    this.reset();
  }

  reset() {
    this.s = {
      rec: 0, t: 0, act: 0, rest: 0, light: 0, mod: 0, vig: 0, enmoSum: 0,
      steps: 0, gait: 0, cadMax: 0, jumps: 0, jumpSum: 0, jumpMax: 0, jlast: 0,
      spr: 0, sprintS: 0, sprintMax: 0, sprintRun: 0, up: 0, dn: 0, alt: 0,
    };
  }

  async connect() {
    this.onStatus('demo');
    this.timer = setInterval(() => this.tick(), 1000);
    this.tick();
  }

  disconnect() {
    clearInterval(this.timer);
    this.onStatus('disconnected');
  }

  async send(command) {
    if (command === 'start' && !this.s.rec) {
      this.reset();
      this.s.rec = 1;
      this.lastId += 1;
    } else if (command === 'stop' && this.s.rec) {
      this.endSprint();
      this.s.rec = 0;
      this.summary = this.buildSummary();
    }
    setTimeout(() => this.tick(false), 150);   // reply quickly, like the pod's next update
  }

  async readSession() {
    return this.summary;
  }

  endSprint() {
    const s = this.s;
    if (s.sprintRun >= 3) {
      s.sprintS += s.sprintRun;
      s.sprintMax = Math.max(s.sprintMax, s.sprintRun);
    }
    s.sprintRun = 0;
  }

  tick(advance = true) {
    const s = this.s;
    let zone = 0, cad = 0, enmo = jitter(0.02, 0.01);
    if (s.rec && advance) {
      const [kind, baseCad, baseEnmo] = SCRIPT[s.t % SCRIPT.length];
      s.t += 1;
      enmo = Math.max(0, jitter(baseEnmo, baseEnmo * 0.15));
      cad = baseCad ? Math.round(jitter(baseCad, 4)) : 0;
      const active = kind !== 'rest';
      zone = !active ? 0 : enmo >= 0.6 ? 3 : enmo >= 0.15 ? 2 : 1;
      if (active) { s.act += 1; s.enmoSum += enmo; } else { s.rest += 1; }
      if (zone === 1) s.light += 1; else if (zone === 2) s.mod += 1; else if (zone === 3) s.vig += 1;
      if (cad) { s.steps += Math.round(cad / 60); s.gait += 1; s.cadMax = Math.max(s.cadMax, cad); }
      if (kind === 'jumps' && s.t % 2 === 0) {
        s.jlast = round1(jitter(24, 9));
        s.jumps += 1; s.jumpSum += s.jlast; s.jumpMax = Math.max(s.jumpMax, s.jlast);
      }
      if (kind === 'sprint') {
        s.sprintRun += 1;
        if (s.sprintRun === 3) s.spr += 1;
      } else {
        this.endSprint();
      }
      // A gentle hill: climb for the first half of each minute, then come back down
      const climbing = (s.t % 60) < 30;
      const step = climbing ? 0.15 : -0.12;
      s.alt += step;
      if (step > 0) s.up += step; else s.dn -= step;
    }
    this.onLive({
      rec: s.rec, t: s.t, act: s.act, rest: s.rest, zone, enmo: Math.round(enmo * 100) / 100,
      steps: s.steps, cad, jumps: s.jumps, jlast: s.jlast, spr: s.spr,
      up: round1(s.up), dn: round1(s.dn), alt: round1(s.alt), baro: 1, last: this.lastId,
    });
  }

  buildSummary() {
    const s = this.s;
    return {
      id: this.lastId, dur: s.t, act: s.act, rest: s.rest, light: s.light, mod: s.mod, vig: s.vig,
      enmo: s.act ? Math.round((s.enmoSum / s.act) * 100) / 100 : 0,
      steps: s.steps, gait: s.gait,
      cad_avg: s.gait ? Math.round((s.steps * 60) / s.gait) : 0, cad_max: s.cadMax,
      jumps: s.jumps, jump_max: s.jumpMax, jump_avg: s.jumps ? round1(s.jumpSum / s.jumps) : 0,
      sprints: s.spr, sprint_s: s.sprintS, sprint_max: s.sprintMax,
      up: round1(s.up), dn: round1(s.dn), baro: 1,
    };
  }
}
