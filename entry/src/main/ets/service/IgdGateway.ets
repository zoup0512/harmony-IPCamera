import { connection, socket } from '@kit.NetworkKit';
import { cert } from '@kit.DeviceCertificateKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { IgdEndpoint, LeaseRoute, LeaseTransport, ipv4, privateHost, sameRoute, leaseAction } from '../model/IgdLease';

function blocked(): never { throw new Error('Gateway verification failed'); }
function subnet(ip: number, prefix: number): number { return Math.floor(ip / Math.pow(2, 32 - prefix)); }
export function wifiRoute(netId: number, caps: connection.NetCapabilities, p: connection.ConnectionProperties, host: string): LeaseRoute {
  if (netId <= 0 || !p.interfaceName || caps.bearerTypes.length !== 1 || caps.bearerTypes[0] !== connection.NetBearType.BEARER_WIFI ||
    !privateHost(host)) blocked();
  const gw = ipv4(host);
  const routes = p.routes.filter((r: connection.RouteInfo) => !r.destination.address.address.includes(':'));
  const defaults = routes.filter((r: connection.RouteInfo) => r.isDefaultRoute || r.destination.prefixLength === 0);
  if (defaults.length !== 1) blocked();
  const d = defaults[0];
  if (!d.isDefaultRoute || d.destination.address.address !== '0.0.0.0' || d.destination.prefixLength !== 0 ||
    !d.hasGateway || d.gateway.address !== host || d.interface !== p.interfaceName || d.isExcludedRoute) blocked();
  const links = p.linkAddresses.filter((a: connection.LinkAddress) => !a.address.address.includes(':'));
  if (links.length !== 1) blocked();
  const addr = links[0], local = addr.address.address, n = addr.prefixLength;
  if (!Number.isInteger(n) || n < 1 || n > 30 || !privateHost(local) || host === local) blocked();
  const ip = ipv4(local), size = Math.pow(2, 32 - n);
  if (subnet(ip, n) !== subnet(gw, n) || Math.min(ip % size, gw % size) === 0 || Math.max(ip % size, gw % size) === size - 1) blocked();
  const direct = routes.filter((r: connection.RouteInfo) => {
    const bits = r.destination.prefixLength;
    if (!Number.isInteger(bits) || bits < 0 || bits > 32) blocked();
    return bits > 0 && subnet(ipv4(r.destination.address.address), bits) === subnet(gw, bits);
  });
  if (direct.length !== 1 || direct[0].hasGateway || direct[0].isExcludedRoute || direct[0].interface !== p.interfaceName ||
    direct[0].destination.prefixLength !== n || ipv4(direct[0].destination.address.address) !== ip - ip % size) blocked();
  return { netId: netId, iface: p.interfaceName, local: local, gateway: host, prefix: n };
}

// Only bounded ASCII XML with explicit HTTP framing. No HTTP client, redirect, proxy, compressed body or replay.
export function xmlReply(data: string): string | undefined {
  const i = data.indexOf('\r\n\r\n');
  if (data.length > 73728 || i > 8192 || (i < 0 && data.length > 8192)) blocked();
  if (i < 0) return undefined;
  const headers = data.substring(0, i).split('\r\n');
  if (!/^HTTP\/1\.[01] 200(?: [\x20-\x7e]*)?$/.test(headers.shift() ?? '')) blocked();
  const seen = new Map<string, string>();
  for (const line of headers) {
    const h = /^([A-Za-z0-9-]+):[ \t]*([\x20-\x7e]*)$/.exec(line);
    if (!h || seen.has(h[1].toLowerCase())) blocked();
    seen.set(h[1].toLowerCase(), h[2].trim());
  }
  if (seen.has('transfer-encoding') || seen.has('content-encoding') || seen.has('location') ||
    !/^(?:text|application)\/xml(?:;\s*charset="?utf-8"?)?$/i.test(seen.get('content-type') ?? '')) blocked();
  const len = seen.get('content-length') ?? '';
  if (!/^[1-9][0-9]{0,4}$/.test(len) || Number(len) > 65536) blocked();
  const body = data.substring(i + 4);
  if (body.length > Number(len)) blocked();
  return body.length === Number(len) ? body : undefined;
}

export class IgdGateway implements LeaseTransport {
  route(host: string): LeaseRoute {
    const net = connection.getDefaultNetSync();
    return wifiRoute(net.netId, connection.getNetCapabilitiesSync(net), connection.getConnectionPropertiesSync(net), host);
  }
  async exchange(route: LeaseRoute, ep: IgdEndpoint, pin: string, path: string, body: string, check: () => void,
    beforeSend: () => void): Promise<string> {
    check();
    const net = connection.getDefaultNetSync();
    if (net.netId !== route.netId || route.gateway !== ep.host) blocked();
    const tcp = socket.constructTCPSocketInstance();
    let tls: socket.TLSSocket | undefined;
    let dead = false, sent = false, timer = -1, data = '';
    const guard = (): void => {
      if (dead || !sameRoute(route, this.route(ep.host))) blocked();
      check();
    };
    const close = (): void => {
      if (tls) tls.close().catch(() => {}); else tcp.close().catch(() => {});
    };
    try {
      return await new Promise<string>((resolve, reject) => {
        const fail = (): void => { dead = true; reject(new Error('Gateway exchange failed')); };
        timer = setTimeout(fail, 5000);
        const run = async (): Promise<void> => {
          const remote: socket.NetAddress = { address: ep.host, port: ep.port, family: 1 };
          await tcp.bind({ address: route.local, port: 0, family: 1 }); guard();
          await net.bindSocket(tcp); guard();
          await tcp.connect({ address: remote, timeout: 3000 }); guard();
          if ((await tcp.getLocalAddress()).address !== route.local) blocked();
          guard(); tls = socket.constructTLSSocketInstance(tcp);
          tls.on('error', fail); tls.on('close', fail);
          tls.on('message', (m: socket.SocketMessageInfo) => {
            if (dead) return;
            try {
              if (!sent || data.length + m.message.byteLength > 73728) blocked();
              const bytes = new Uint8Array(m.message);
              for (const b of bytes) {
                if (b > 126 || (b < 32 && b !== 9 && b !== 10 && b !== 13)) blocked();
                data += String.fromCharCode(b);
              }
              const result = xmlReply(data);
              if (result !== undefined) { dead = true; resolve(result); }
            } catch (_) { fail(); }
          });
          // Out-of-band leaf pin is mandatory on BOTH connections; never send HTTP under unverified TLS.
          await tls.connect({ address: remote, timeout: 3000, skipRemoteValidation: false,
            secureOptions: { protocols: [socket.Protocol.TLSv12, socket.Protocol.TLSv13] } });
          const peer = await tls.getRemoteCertificate();
          const der = await (await cert.createX509Cert(peer)).getEncoded();
          if (der.encodingFormat !== cert.EncodingFormat.FORMAT_DER) blocked();
          const md = cryptoFramework.createMd('SHA256'); md.updateSync({ data: der.data });
          const actual = Array.from(md.digestSync().data).map((n: number) => n.toString(16).padStart(2, '0')).join('');
          if (pin !== actual) blocked();
          const first = `${body ? 'POST' : 'GET'} ${path} HTTP/1.1\r\nHost: ${ep.host}:${ep.port}\r\nConnection: close\r\n`;
          const request = first + (body ? `Content-Type: text/xml; charset=utf-8\r\nSOAPAction: "${leaseAction}"\r\nContent-Length: ${body.length}\r\n` : '') + '\r\n' + body;
          guard(); beforeSend(); sent = true;
          await tls.send(request);
        };
        run().catch(() => { fail(); close(); }); // Also release a socket acquired after a deadline.
      });
    } finally { dead = true; clearTimeout(timer); close(); }
  }
}
