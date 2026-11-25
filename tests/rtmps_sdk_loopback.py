"""Windows DevEco SDK: python tests/rtmps_sdk_loopback.py 127.0.0.1:<emulator-hdc-port>
Requires a local API 24 emulator without the app installed and its debug signing configuration.
No root store changes. Only encrypted test traffic crosses HDC, between checked loopback listeners.
"""
import argparse
import json
import os
from pathlib import Path
import re
import runpy
import secrets
import shutil
import socket
import ssl
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
TEST = ROOT/'tests'
PROTO = runpy.run_path(str(TEST/'rtmp_publisher_loopback_test.py'))
BUNDLE = 'com.shenyaocn.harmony.ipcamera'


def build(p, studio, port, tag):
    for rel in ('AppScope', 'hvigor', 'entry/src/main/resources', 'entry/src/main/cpp'):
        shutil.copytree(ROOT/rel, p/rel)
    for rel in ('build-profile.json5', 'hvigorfile.ts', 'oh-package.json5', 'entry/build-profile.json5',
                'entry/hvigorfile.ts', 'entry/oh-package.json5', 'entry/oh-package-lock.json5', 'entry/src/main/module.json5'):
        shutil.copy2(ROOT/rel, p/rel)
    shutil.copytree(p/'entry/src/main/cpp/types/libstreaming', p/'entry/oh_modules/libstreaming.so')
    ets = p/'entry/src/main/ets'; util = ets/'util'; util.mkdir(parents=True)
    (ets/'pages').mkdir(); (ets/'entryability').mkdir()
    shutil.copy2(TEST/'rtmps_sdk_probe.ets', ets/'entryability/EntryAbility.ets')
    (ets/'pages/Index.ets').write_text('@Entry @Component struct Index { build() { Text("Loopback probe") } }')
    (p/'entry/src/main/resources/base/profile/main_pages.json').write_text('{"src":["pages/Index"]}')
    for f in ('RtmpTlsIdentity.ts','RtmpTlsTransport.ets'):
        shutil.copy2(ROOT/'entry/src/main/ets/util'/f, util/f)
    sslcmd = shutil.which('openssl') or 'C:/Program Files/Git/usr/bin/openssl.exe'
    def openssl(*args):
        subprocess.run([sslcmd, *args], cwd=p, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    openssl('req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=RTMPS disposable test root',
            '-addext','basicConstraints=critical,CA:TRUE','-keyout','root.key','-out','ca.pem')
    for name,san in [('good','DNS:localhost,IP:127.0.0.1,IP:::1'), ('bad','DNS:example.invalid,DNS:127.0.0.1,IP:127.0.0.2')]:
        openssl('req','-newkey','rsa:2048','-nodes','-subj','/CN=localhost','-keyout',name+'.key','-out',name+'.csr')
        (p/'ext.txt').write_text('subjectAltName='+san+'\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n')
        openssl('x509','-req','-days','1','-in',name+'.csr','-CA','ca.pem','-CAkey','root.key','-CAcreateserial',
                '-extfile','ext.txt','-out',name+'.pem')
    keys = {'caPem':(p/'ca.pem').read_text(), 'goodCert':(p/'good.pem').read_text(), 'goodKey':(p/'good.key').read_text()}
    (util/'LabKeys.ets').write_text('\n'.join('export const '+k+' = '+json.dumps(v)+';' for k,v in keys.items())+
                                  f'\nexport const port = {port};\nexport const runId = "{tag}";\n')
    src = (util/'RtmpTlsTransport.ets').read_text(); needle = 'secureOptions: {},'
    assert src.count(needle) == 1
    (util/'LabTransport.ets').write_text("import { caPem } from './LabKeys';\n"+src.replace(needle,'secureOptions: {ca:caPem},'))
    env = dict(os.environ, JAVA_HOME=str(studio/'jbr'), DEVECO_SDK_HOME=str(studio/'sdk'))
    env['PATH'] = str(studio/'jbr/bin') + os.pathsep + env['PATH']
    subprocess.run([studio/'tools/node/node.exe',studio/'tools/hvigor/bin/hvigorw.js','--mode','module','-p','product=default',
                    'assembleHap','--offline','--no-daemon'],cwd=p,env=env,check=True)
    return p/'entry/build/default/outputs/default/entry-default-signed.hap'


def exercise(s, p):
    for mode in ('untrusted','wrong-san','wrong-dns','ipv4','dns','cancel','timeout','tls-cancel','tls-timeout'):
        print('oracle:',mode,flush=True)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        key = 'bad' if mode in ('wrong-san','wrong-dns') else 'good'; ctx.load_cert_chain(p/(key+'.pem'), p/(key+'.key'))
        raw, _ = s.accept(); raw.settimeout(13)
        with raw:
            if mode in ('tls-cancel','tls-timeout'):
                assert raw.recv(4096).startswith(b''), 'expected encrypted handshake start'
                # On an SDK whose in-flight handshake does not abort immediately,
                # do not let one peer stall the listener needed for the next case.
                if mode == 'tls-cancel': time.sleep(.55)
                else: time.sleep(10.2)
                continue
            try:
                c = ctx.wrap_socket(raw,server_side=True)
            except (ssl.SSLError, ConnectionResetError):
                if mode != 'untrusted': raise
                continue
            with c:
                if mode in ('untrusted','wrong-san','wrong-dns'):
                    try: assert c.recv(1) == b'', 'unauthenticated RTMP bytes'
                    except (ConnectionResetError, ssl.SSLError): pass
                    continue
                exact, message, wire, cmd, info, num = (PROTO[k] for k in ('exact','message','wire','cmd','info','num'))
                c1 = exact(c,1537); assert c1[0] == 3
                if mode in ('cancel','timeout'):
                    assert c.recv(1) == b'', 'late writes after stop'; continue
                s1 = bytes(range(256))*6; c.sendall(b'\x03'+s1+c1[1:]); assert exact(c,1536) == s1
                assert message(c)[:2] == (1,0)
                body = message(c)[2]; assert b'rtmps://' in body and b'synthetic-key' not in body
                c.sendall(wire(cmd('_result',1,info('NetConnection.Connect.Success')),chunk=128))
                assert b'createStream' in message(c)[2]; c.sendall(wire(cmd('_result',2,num(1)),chunk=128))
                typ,sid,body = message(c); assert (typ,sid) == (20,1) and b'synthetic-key' in body
                c.sendall(wire(cmd('onStatus',0,info('NetStream.Publish.Start')),sid=1,chunk=128))
                for expected in (18,9,9,8,8):
                    typ, sid, body = message(c); assert (typ,sid) == (expected,1) and body
                while c.recv(4096): pass


def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('emulator'); args=ap.parse_args()
    if not re.fullmatch(r'127\.0\.0\.1:[0-9]{1,5}',args.emulator): ap.error('local emulator only')
    studio = Path(os.environ['DEVECO_SDK_HOME']).parent; hdc = studio/'sdk/default/openharmony/toolchains/hdc.exe'
    def h(*args):
        return subprocess.check_output([hdc,'-t',args_emulator,*args],timeout=30,text=True,encoding='utf-8',errors='replace').strip()
    args_emulator = args.emulator
    assert h('shell','param get const.product.model').strip() == 'emulator', 'not an emulator'
    assert BUNDLE not in h('shell','bm dump -a'), 'refusing to overwrite an installed app'
    tag = secrets.token_hex(6)
    with tempfile.TemporaryDirectory(prefix='rtmps-sdk-') as tmp, socket.socket() as listener:
        listener.bind(('127.0.0.1',0)); listener.listen(4); listener.settimeout(20)
        port=listener.getsockname()[1]; node='tcp:'+str(port); p=Path(tmp)
        hap=build(p,studio,port,tag)
        assert 'successfully' in h('install',str(hap)); forwarded=False
        try:
            assert 'OK' in h('rport',node,node); forwarded=True
            rows=h('shell','netstat -lnt').splitlines(); bound=[r.split()[3] for r in rows if r.rstrip().endswith('LISTEN') and r.split()[3].endswith(':'+str(port))]
            assert bound == ['127.0.0.1:'+str(port)], 'test forward is not exclusively loopback'
            assert 'successfully' in h('shell',f'aa start -b {BUNDLE} -a EntryAbility')
            exercise(listener,p)
            for _ in range(25):
                lines=[l for l in h('shell','hilog -x -T RTMPSTEST').splitlines() if tag in l]
                if any('FAIL' in l for l in lines): raise AssertionError(lines)
                if any('ALL PASS' in l for l in lines): print('\n'.join(lines)); break
                time.sleep(.25)
            else: raise AssertionError('missing SDK pass signal: '+str(lines))
        finally:
            if forwarded: h('fport','rm',node,node)
            h('uninstall',BUNDLE)

if __name__=='__main__': main()
