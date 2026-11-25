// node --test tests/rtmp_retry_test.cjs (Node 22.18+); no wall-clock or network use.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { RtmpRetryController } = require('./rtmp_host_modules.cjs');

const address = 'rtmp://example.test/live/private-key?token=private-token';
const startup = 'startup failed: connect failed: Connection refused';
const lost = 'rtmp: connection lost: server closed connection';
const denied = 'connection lost: server rejected: NetStream.Publish.Denied';
const tick = () => new Promise(resolve => setImmediate(resolve));

function rig() {
  const starts = [], states = [], timers = new Map();
  let id = 0, stops = 0, unsettled = 0;
  const c = new RtmpRetryController({
    start: url => {
      assert.equal(++unsettled, 1, 'native starts must never overlap');
      return new Promise((resolve, reject) => {
        starts.push({ url, ok: (value = true) => { --unsettled; resolve(value); },
          fail: msg => { --unsettled; reject(new Error(msg)); } });
      });
    },
    stop: () => { ++stops; },
    changed: (...state) => states.push(state)
  }, {
    after: (task, ms) => {
      assert.equal(timers.size, 0, 'at most one scheduled retry');
      timers.set(++id, { task, ms });
      return id;
    },
    cancel: i => timers.delete(i)
  });
  return { c, starts, states, timers, stops: () => stops, phase: () => states.at(-1)?.[0],
    fire: () => {
      assert.equal(timers.size, 1);
      const [i, timer] = timers.entries().next().value;
      timers.delete(i);
      timer.task();
      return timer;
    } };
}

for (const eventFirst of [true, false]) {
  test(`one retry for status + start rejection (event first: ${eventFirst})`, async () => {
    const r = rig();
    r.c.start(address);
    r.c.start(address);
    assert.equal(r.starts.length, 1);
    if (eventFirst) r.c.nativeError(`rtmp: ${startup}`);
    r.starts[0].fail(startup);
    await tick();
    if (!eventFirst) r.c.nativeError(`rtmp: ${startup}`);
    r.c.start(address);
    assert.deepEqual(r.states.at(-1), ['waiting', 1, 1000]);
    assert.equal(r.stops(), 1);
    const timer = r.fire();
    r.starts[1].ok();
    await tick();
    assert.equal(r.phase(), 'publishing');
    r.c.nativeError(lost);
    r.c.nativeError(lost); // snapshot + event
    timer.task(); // stale timer must not consume the second retry
    assert.equal(r.starts.length, 2);
    assert.deepEqual(r.states.at(-1), ['waiting', 2, 2000]);
  });
}

test('runtime disconnect before successful promise delivery never reports publishing', async () => {
  const r = rig();
  r.c.start(address);
  r.c.nativeError(lost);
  assert.equal(r.timers.size, 0);
  r.starts[0].ok();
  await tick();
  assert.deepEqual(r.states.map(s => s[0]), ['connecting', 'waiting']);
});

test('repeated early successes cannot refill the finite backoff budget', async () => {
  const r = rig();
  r.c.start(address);
  const delays = [];
  for (let i = 0; i < 5; ++i) {
    r.starts[i].ok();
    await tick();
    r.c.nativeError(lost);
    if (i < 4) delays.push(r.fire().ms);
  }
  assert.deepEqual(delays, [1000, 2000, 4000, 8000]);
  assert.equal(r.phase(), 'idle');
  assert.equal(r.timers.size, 0);
  r.c.start(address); // manual start gets a new budget
  r.starts[5].fail(startup);
  await tick();
  assert.deepEqual(r.states.at(-1), ['waiting', 1, 1000]);
});

test('cancel waiting timer even after its callback has already been queued', async () => {
  const r = rig();
  r.c.start(address);
  r.starts[0].fail(startup);
  await tick();
  const queued = [...r.timers.values()][0];
  r.c.stop(); // same stop for button, address edit, page exit and service stop
  queued.task();
  r.c.nativeError(lost);
  r.c.stop();
  assert.equal(r.phase(), 'idle');
  assert.equal(r.starts.length, 1);
  assert.equal(r.timers.size, 0);
});

test('pending cancellation and address replacement serialize native workers', async () => {
  const r = rig();
  r.c.start(address);
  r.c.start('rtmp://new/live/one');
  r.c.start('rtmp://new/live/two');
  assert.equal(r.starts.length, 1);
  r.starts[0].fail(startup); // stale completion cannot stop/retry the new session
  await tick();
  assert.deepEqual(r.starts.map(s => s.url), [address, 'rtmp://new/live/two']);
  r.c.stop();
  r.starts[1].ok();
  await tick();
  assert.equal(r.phase(), 'idle');
  assert.equal(r.timers.size, 0);
});

test('explicit rejection, protocol, cancellation and unknown errors never retry or leak', async () => {
  for (const reason of [denied, 'startup failed: unsupported RTMP handshake version',
    'startup failed: invalid RTMP port', 'cancelled', `unexpected error: ${address}`]) {
    const r = rig();
    r.c.start(address);
    r.starts[0].fail(reason);
    await tick();
    assert.equal(r.phase(), 'idle');
    assert.equal(r.timers.size, 0);
    assert.doesNotMatch(JSON.stringify(r.states), /private|rtmp:\/\//);
  }
  const r = rig();
  r.c.start(address);
  r.starts[0].ok();
  await tick();
  r.c.nativeError('rtmp: ' + denied);
  assert.equal(r.phase(), 'idle');
  r.c.start('rtmps://user:password@example/live/key');
  assert.equal(r.starts.length, 1);
});

test('RTMPS retries only identified transport failures, always with the original secure URL', async () => {
  const secure = 'rtmps://CaM.Example:8443/live/private%2Fkey?token=private';
  const r = rig();
  r.c.start(secure);
  const delays = [];
  for (let i = 0; i < 5; ++i) {
    assert.equal(r.starts[i].url, secure);
    r.starts[i].fail([startup, 'RTMPS connection closed', 'RTMPS write timed out', 'RTMPS write failed',
      'connection lost: recv failed: Connection reset by peer'][i]);
    await tick();
    if (i < 4) delays.push(r.fire().ms);
  }
  assert.deepEqual(delays, [1000, 2000, 4000, 8000]);
  assert.equal(r.timers.size, 0);
  assert.equal(r.phase(), 'idle');
  assert.doesNotMatch(JSON.stringify(r.states), /private|rtmps?:\/\//);
});

test('RTMPS verification failure (false), unknown TLS errors and server rejection are terminal', async () => {
  const secure = 'rtmps://[::1]/private/key?token=secret';
  for (const reason of [false, 'startup failed: connect failed: certificate verify failed',
    'RTMPS certificate SAN mismatch', 'RTMPS certificate verification or connection failed',
    'RTMPS invalid transport state', 'RTMPS transport error', 'RTMPS startup timed out', denied]) {
    const r = rig();
    r.c.start(secure);
    if (reason === false) {
      // Even if a close callback arrives first, false is not network confirmation.
      r.c.nativeError(lost);
      r.starts[0].ok(false);
    } else r.starts[0].fail(reason);
    await tick();
    assert.equal(r.phase(), 'idle');
    assert.equal(r.timers.size, 0);
    assert.deepEqual(r.starts.map(s => s.url), [secure]);
    assert.doesNotMatch(JSON.stringify(r.states), /secret|private|rtmps?:\/\//);
  }
});

test('page visibility guard can cancel synchronously on a phase notification', async () => {
  for (const cancelAt of ['connecting', 'waiting']) {
    let starts = 0, armed = false, phase;
    const c = new RtmpRetryController({
      start: () => { ++starts; return Promise.reject(new Error(startup)); },
      stop: () => {},
      changed: p => { phase = p; if (p === cancelAt) c.stop(); }
    }, {
      after: () => { armed = true; return 1; },
      cancel: () => { armed = false; }
    });
    c.start(address);
    await tick();
    assert.equal(phase, 'idle');
    assert.equal(armed, false);
    assert.equal(starts, cancelAt === 'connecting' ? 0 : 1);
  }
});

test('fatal callback wins over a late transport rejection or already scheduled timer', async () => {
  for (const phase of ['connecting', 'waiting']) {
    const r = rig();
    r.c.start(address);
    if (phase === 'waiting') {
      r.starts[0].fail(startup);
      await tick();
    }
    r.c.nativeError('rtmp: ' + denied);
    if (phase === 'connecting') r.starts[0].fail(startup);
    await tick();
    assert.equal(r.phase(), 'idle');
    assert.equal(r.starts.length, 1);
    assert.equal(r.timers.size, 0);
  }
});

test('unsupported replacement cancels a queued retry; cancelled native result is terminal', async () => {
  const r = rig();
  r.c.start(address);
  r.starts[0].fail(startup);
  await tick();
  r.c.start('https://example/live/key');
  assert.equal(r.timers.size, 0);
  r.c.start(address);
  r.starts[1].ok(false);
  await tick();
  assert.equal(r.phase(), 'idle');
  assert.equal(r.timers.size, 0);
});
