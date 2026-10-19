// Talks to the SockPod over Web Bluetooth. The GATT interface is described in README.md.

const SERVICE = 'a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a00';
const LIVE    = 'a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a01';
const SESSION = 'a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a02';
const CONTROL = 'a8f5c3e0-6d2b-4f1a-9c7e-3b5d8e2f1a03';

const decoder = new TextDecoder();
const encoder = new TextEncoder();

// Returns why Bluetooth can't be used here, or null if it can.
export function bleProblem() {
  if (!window.isSecureContext) {
    return 'Bluetooth only works on pages served over https or from localhost.';
  }
  if (!('bluetooth' in navigator)) {
    return 'This browser can\'t use Bluetooth. Use Chrome or Edge on Windows, macOS, ' +
           'Android or ChromeOS (iPhone Safari and Firefox don\'t support it).';
  }
  return null;
}

export class BleTransport {
  source = 'pod';

  // onLive(message) gets each live JSON message; onStatus(text) gets
  // 'connecting' | 'connected' | 'reconnecting' | 'disconnected'.
  constructor({ onLive, onStatus }) {
    this.onLive = onLive;
    this.onStatus = onStatus;
    this.queue = Promise.resolve();   // Chrome allows one GATT operation at a time
    this.manualDisconnect = false;
    this.readingLive = false;
  }

  // GATT operations run one after another
  enqueue(fn) {
    const run = this.queue.then(fn, fn);
    this.queue = run.catch(() => {});
    return run;
  }

  async connect() {
    this.device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE] }, { name: 'SockPod' }],
      optionalServices: [SERVICE],
    });
    this.device.addEventListener('gattserverdisconnected', () => this.handleDisconnect());
    this.manualDisconnect = false;
    await this.open();
  }

  async open() {
    this.onStatus('connecting');
    const server = await this.device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE);
    this.live = await service.getCharacteristic(LIVE);
    this.session = await service.getCharacteristic(SESSION);
    this.control = await service.getCharacteristic(CONTROL);
    // Chrome may hand back the same object after a reconnect, so never add the listener twice
    this.liveListener ??= (e) => this.handleLive(e.target.value);
    this.live.removeEventListener('characteristicvaluechanged', this.liveListener);
    this.live.addEventListener('characteristicvaluechanged', this.liveListener);
    await this.live.startNotifications();
    this.onStatus('connected');
    await this.enqueue(() => this.live.readValue());   // fires characteristicvaluechanged
  }

  handleLive(view) {
    try {
      this.onLive(JSON.parse(decoder.decode(view)));
    } catch {
      // A notification cut short by a small MTU isn't valid JSON. Reads aren't
      // limited by the MTU, so read the full value (that fires this handler again).
      if (this.readingLive) return;
      this.readingLive = true;
      this.enqueue(() => this.live.readValue())
        .catch(() => {})
        .finally(() => { this.readingLive = false; });
    }
  }

  async readSession() {
    const view = await this.enqueue(() => this.session.readValue());
    return JSON.parse(decoder.decode(view));
  }

  async send(command) {
    await this.enqueue(() => this.control.writeValueWithResponse(encoder.encode(command)));
  }

  disconnect() {
    this.manualDisconnect = true;
    if (this.device?.gatt.connected) this.device.gatt.disconnect();
    else this.onStatus('disconnected');
  }

  async handleDisconnect() {
    if (this.manualDisconnect) {
      this.onStatus('disconnected');
      return;
    }
    // Out of range or the pod rebooted: try to get it back for about 20 s
    this.onStatus('reconnecting');
    for (let attempt = 0; attempt < 5 && !this.manualDisconnect; attempt++) {
      await new Promise((r) => setTimeout(r, 1000 + attempt * 1500));
      try {
        await this.open();
        return;
      } catch {
        // try again
      }
    }
    this.onStatus('disconnected');
  }
}
