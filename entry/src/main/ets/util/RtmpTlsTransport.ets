import { socket } from '@kit.NetworkKit';
import { cert } from '@kit.DeviceCertificateKit';
import { rtmpTlsOpen, rtmpTlsClose, rtmpTlsRead, rtmpTlsWrite } from 'libstreaming.so';
import { rtmpTlsEndpoint, RtmpTlsEndpoint } from './RtmpTlsIdentity';

// Failed-to-settle SDK calls must not let repeated cancellations grow an
// unlimited number of sockets, send buffers, closures or pending handshakes.
let liveSessions = 0;
const MAX_SESSIONS = 2;
const START_MS = 10000;
const WRITE_MS = 5000;
type BeginPublish = (authenticatedTlsToken: number) => Promise<boolean>;

class TlsSession {
  private tls: socket.TLSSocket = socket.constructTLSSocketInstance();
  private token: number = 0;
  private dead: boolean = false;
  reason: string = '';
  private timer: number = -1;
  private pumpTimer: number = -1;
  private sendTimer: number = -1;
  private outcome?: (value: boolean) => void;
  private startup?: Promise<void>;
  private sending?: Promise<void>;

  start(ep: RtmpTlsEndpoint, begin: BeginPublish): Promise<boolean> {
    const result = new Promise<boolean>((resolve) => { this.outcome = resolve; });
    this.timer = setTimeout(() => this.stop('RTMPS startup timed out'), START_MS);
    this.startup = this.authenticate(ep, begin).catch(() => this.stop(this.token ? '' : 'RTMPS certificate verification or connection failed'));
    return result;
  }

  private async authenticate(ep: RtmpTlsEndpoint, begin: BeginPublish): Promise<void> {
    await this.tls.bind({ address: ep.family === 2 ? '::' : '0.0.0.0', family: ep.family, port: 0 });
    if (this.dead) return;
    this.tls.on('close', () => this.stop(this.token ? 'RTMPS connection closed' : 'RTMPS certificate verification or connection failed'));
    this.tls.on('error', () => this.stop(this.token ? 'RTMPS transport error' : 'RTMPS certificate verification or connection failed'));
    this.tls.on('message', (info: socket.SocketMessageInfo) => {
      if (this.dead) return;
      // RTMP peers must wait for client C0/C1. Never buffer pre-auth input.
      if (!this.token) { this.stop('RTMPS invalid transport state'); return; }
      // API 24 may return a typed view rather than the declared ArrayBuffer.
      // Bound the copy first, then pass an exact, plain ArrayBuffer to NAPI.
      const size = info.message.byteLength;
      if (!size || size > 256 * 1024) { this.stop('RTMPS invalid input size'); return; }
      try {
        const bytes = new Uint8Array(info.message);
        if (bytes.byteLength !== size) { this.stop('RTMPS invalid input'); return; }
        rtmpTlsWrite(this.token, bytes.slice().buffer);
      } catch (_) { this.stop('RTMPS transport error'); }
    });
    // Omit ca: API 12+ defaults to the system's preset CA certificates.
    await this.tls.connect({ address: { address: ep.host, family: ep.family, port: ep.port },
      secureOptions: {}, skipRemoteValidation: false, timeout: START_MS });
    if (this.dead) return;
    const remote = await this.tls.getRemoteCertificate();
    if (this.dead) return;
    const leaf = await cert.createX509Cert(remote);
    if (this.dead) return;
    const name: cert.GeneralName = { type: ep.dns ? cert.GeneralNameType.GENERAL_NAME_TYPE_DNS_NAME :
      cert.GeneralNameType.GENERAL_NAME_TYPE_IP_ADDRESS, name: ep.generalName };
    if (!leaf.match({ subjectAlternativeNames: [name], matchAllSubjectAltNames: false })) { this.stop('RTMPS certificate SAN mismatch'); return; }
    this.token = rtmpTlsOpen(ep.authority);
    if (!this.token) { this.stop('RTMPS invalid transport state'); return; }
    // Only now can native acquire the in-process socket. Its own result means
    // NetStream.Publish.Start, not merely a successful TCP/TLS connection.
    this.schedule(0);
    const ok = await begin(this.token);
    if (this.dead) return;
    if (!ok) { this.stop(''); return; }
    clearTimeout(this.timer); this.timer = -1;
    this.finish(true);
  }

  private finish(ok: boolean): void {
    const resolve = this.outcome; this.outcome = undefined;
    if (resolve) resolve(ok);
  }
  private schedule(delay: number): void {
    this.pumpTimer = setTimeout(() => {
      this.pumpTimer = -1;
      if (this.dead) return;
      try {
        const data = rtmpTlsRead(this.token);
        if (!data) { this.schedule(10); return; }
        this.sendTimer = setTimeout(() => this.stop('RTMPS write timed out'), WRITE_MS);
        this.sending = this.tls.send(data).then(() => {
          clearTimeout(this.sendTimer); this.sendTimer = -1;
          if (!this.dead) this.schedule(0);
        }).catch(() => this.stop('RTMPS write failed'));
      } catch (_) { this.stop('RTMPS transport error'); }
    }, delay);
  }

  stop(reason: string = 'cancelled'): void {
    if (this.dead) return;
    this.dead = true; this.reason = reason;
    clearTimeout(this.timer); clearTimeout(this.pumpTimer); clearTimeout(this.sendTimer);
    // Wake native poll immediately, not when SDK close or a JS promise settles.
    if (this.token) { rtmpTlsClose(this.token); this.token = 0; }
    this.finish(false);
    this.release().catch(() => {});
  }
  private async release(): Promise<void> {
    // Interrupt pending operations; a late bind/connect may still acquire a
    // resource. Close that same socket again after pending work settles.
    try { await this.tls.close(); } catch (_) {}
    if (this.startup) await this.startup;
    if (this.sending) await this.sending;
    try { await this.tls.close(); } catch (_) {}
    liveSessions--;
  }
}

export class RtmpTlsTransport {
  private session?: TlsSession;
  private lastError: string = '';
  failure(): string { return this.session ? this.session.reason : this.lastError; }
  start(url: string, startNative: BeginPublish): Promise<boolean> {
    this.stop();
    this.lastError = '';
    if (liveSessions >= MAX_SESSIONS) { this.lastError = 'RTMPS transport busy'; return Promise.resolve(false); }
    try {
      const ep = rtmpTlsEndpoint(url);
      this.session = new TlsSession(); liveSessions++;
      return this.session.start(ep, startNative);
    } catch (_) { this.stop(); this.lastError = 'invalid RTMPS endpoint or transport unavailable'; return Promise.resolve(false); }
  }
  stop(): void {
    const old = this.session; this.session = undefined;
    if (old) { old.stop(); this.lastError = old.reason; }
  }
}
