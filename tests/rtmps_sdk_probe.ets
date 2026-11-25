// Disposable SDK HAP entrypoint, built only by rtmps_sdk_loopback.py.
import { UIAbility } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { socket } from '@kit.NetworkKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RtmpTlsTransport } from '../util/RtmpTlsTransport';
import { RtmpTlsTransport as LabTransport } from '../util/LabTransport';
import { createRtspServer, rtmpStart, rtmpStop, rtspSendH264, rtspSendAdts } from 'libstreaming.so';
import { caPem, goodCert, goodKey, port, runId } from '../util/LabKeys';

const wait = (ms: number): Promise<void> => new Promise<void>((resolve) => { setTimeout(resolve, ms); });
async function scenario(mode: string): Promise<void> {
  const prod = new RtmpTlsTransport(); const lab = new LabTransport(); const h = createRtspServer();
  const v6 = mode === 'ipv6-san';
  // IPv6 SAN check uses an emulator-local official TLS server. Its result
  // proves authentication only and is never reported as a successful publish.
  let local: socket.TLSSocketServer | undefined = undefined;
  if (v6) {
    local = socket.constructTLSSocketServerInstance();
    await local.listen({address:{address:'::1', port, family:2},secureOptions:{ca:caPem,cert:goodCert,key:goodKey}});
    local.on('error', () => {}); local.on('connect', (c: socket.TLSSocketConnection) => { c.on('error', () => {}); });
  }
  let starts = 0;
  const url = `rtmps://${v6 ? '[::1]' : (mode === 'dns' || mode === 'wrong-dns' ? 'localhost' : '127.0.0.1')}:${port}/live/synthetic-key`;
  const begin = (token: number): Promise<boolean> => {
    starts++; return v6 ? Promise.resolve(false) : rtmpStart(h, url, token);
  };
  try {
    const at = Date.now();
    const pending = mode === 'untrusted' ? prod.start(url, begin) : lab.start(url, begin);
    if (mode === 'tls-cancel') { await wait(250); lab.stop(); }
    if (mode === 'cancel') { for (let i = 0; i < 100 && !starts; i++) await wait(10); await wait(50); lab.stop(); }
    const ok = await pending;
    const elapsed = Date.now() - at;
    let pass = false;
    if (mode === 'untrusted' || mode === 'wrong-san' || mode === 'wrong-dns') pass = !ok && !starts;
    else if (mode === 'tls-cancel') pass = !ok && !starts && elapsed < 2500;
    else if (mode === 'tls-timeout') pass = !ok && !starts && elapsed >= 8000 && elapsed < 12000;
    else if (mode === 'cancel') pass = !ok && starts === 1 && elapsed < 2500;
    else if (mode === 'timeout') pass = !ok && starts === 1 && elapsed > 8000 && elapsed < 12000;
    else if (v6) pass = !ok && starts === 1;
    else pass = ok && starts === 1;
    hilog.info(0, 'RTMPSTEST', '%{public}s %{public}s %{public}s starts=%{public}d ms=%{public}d', runId, mode, pass ? 'PASS':'FAIL', starts, elapsed);
    if (pass && ok) {
      rtspSendH264(h, new Uint8Array([0,0,1,0x67,0x42,0,0x1f,0,0,1,0x68,1,0,0,1,0x65,9]).buffer, 1000000);
      rtspSendAdts(h, new Uint8Array([0xff,0xf1,0x4c,0x40,1,0x1f,0xfc,0xac]).buffer, 1000000);
      await wait(500);
    }
    if (!pass) throw new Error('unexpected outcome');
  } finally { prod.stop(); lab.stop(); rtmpStop(h); if (local) await local.close(); await wait(250); }
}
async function suite(): Promise<void> {
  for (const mode of ['untrusted','wrong-san','wrong-dns','ipv4','dns','cancel','timeout','tls-cancel','tls-timeout','ipv6-san']) await scenario(mode);
  hilog.info(0,'RTMPSTEST','%{public}s ALL PASS',runId);
}
export default class EntryAbility extends UIAbility {
  onWindowStageCreate(stage: window.WindowStage): void {
    stage.loadContent('pages/Index'); suite().catch(() => hilog.error(0,'RTMPSTEST','%{public}s FAIL',runId));
  }
}
