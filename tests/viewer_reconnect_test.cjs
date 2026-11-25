// node --test tests/viewer_reconnect_test.cjs (Node 22.18+). No device, network or wall clock.
const test = require('node:test');
const assert = require('node:assert/strict');
const { ViewerReconnect, rtspViewerFailure, httpViewerFailure, httpViewerRequestFailure } =
  require('../entry/src/main/ets/util/ViewerReconnect.ts');

function rig() {
  const pending = new Map(), starts = [];
  let next = 0;
  const view = new ViewerReconnect({
    after(task, ms) {
      assert.equal(pending.size, 0);
      const id = next++;
      pending.set(id, { task, ms });
      return id;
    },
    cancel(id) { pending.delete(id); }
  });
  const open = id => { assert.ok(view.current(id)); starts.push(id); };
  function fire() {
    assert.equal(pending.size, 1);
    const [[id, timer]] = pending;
    pending.delete(id);
    timer.task();
    return timer;
  }
  return { view, pending, starts, open, fire };
}

test('opt-in and a previously playing stream are both required', () => {
  for (const optedIn of [false, true]) {
    const { view, open, starts, pending } = rig();
    view.setForeground(true);
    if (optedIn) view.start(open, true); else view.start(open);
    if (!optedIn) view.playing(starts[0]);
    assert.equal(view.failed(starts[0], 'disconnect'), 0);
    assert.equal(view.current(starts[0]), false);
    assert.equal(pending.size, 0);
  }
});

test('one timer per attempt, bounded across both failed reconnects and short successes', () => {
  const { view, open, starts, pending, fire } = rig();
  view.setForeground(true);
  view.start(open, true);
  view.playing(starts[0]);
  for (const delay of [1000, 2000, 4000]) {
    const id = starts.at(-1);
    assert.equal(view.failed(id, 'disconnect'), delay);
    assert.equal(view.current(id), false);
    assert.equal(view.failed(id, 'disconnect'), 0);
    const old = fire();
    assert.equal(old.ms, delay);
    assert.notEqual(starts.at(-1), id);
    const count = starts.length;
    old.task(); // Dequeued twice must not launch another connect.
    view.failed(id); // Old transport reports auth failure after new transport starts.
    assert.equal(starts.length, count);
    assert.ok(view.current(starts.at(-1)));
    if (delay !== 1000) view.playing(starts.at(-1));
  }
  assert.equal(view.failed(starts.at(-1), 'disconnect'), 0);
  assert.equal(starts.length, 4);
  assert.equal(pending.size, 0);
  view.start(open, true); // Only explicit Play/Retry gives a fresh budget.
  view.playing(starts.at(-1));
  assert.equal(view.failed(starts.at(-1), 'disconnect'), 1000);
});

test('stopping, hiding or replacing a source revokes even a dequeued timer and stale callbacks', () => {
  for (const action of ['cancel', 'hide', 'replace']) {
    const { view, open, starts, pending } = rig();
    view.start(open, true); // Foreground is explicit.
    assert.equal(starts.length, 0);
    view.setForeground(true);
    view.start(open, true);
    const old = starts[0];
    view.playing(old);
    view.failed(old, 'disconnect');
    const late = [...pending.values()][0].task;
    if (action === 'cancel') view.cancel();
    if (action === 'hide') {
      view.setForeground(false);
      view.start(open, true);
      late();
      view.setForeground(true);
    }
    if (action === 'replace') view.start(open, true);
    late();
    view.playing(old);
    view.failed(old, 'disconnect');
    assert.equal(view.current(old), false);
    assert.equal(starts.length, action === 'replace' ? 2 : 1);
    if (action === 'replace') {
      const fresh = starts[1];
      assert.ok(view.current(fresh));
      assert.equal(view.failed(fresh, 'disconnect'), 0, 'old frames cannot establish the new source');
    }
    assert.equal(pending.size, 0);
  }
});

test('HTTP and RTSP allowlists reject auth, decode, protocol, redirect, certificate and unknown errors', () => {
  for (const code of [-1, -7]) assert.equal(rtspViewerFailure(code), 'disconnect');
  for (const code of [408, 502, 503, 504]) assert.equal(httpViewerFailure(code), 'disconnect');
  for (const code of [2300006, 2300007, 2300018, 2300028, 2300052, 2300055, 2300056]) {
    assert.equal(httpViewerRequestFailure(code), 'disconnect');
  }
  for (const [classify, codes] of [[rtspViewerFailure, [-2, -3, -4, -5, -6, -8, -99]],
    [httpViewerFailure, [301, 307, 401, 403, 404, 407, 415, 429, 501, 505]],
    [httpViewerRequestFailure, [2300001, 2300047, 2300060, 999]]]) {
    for (const code of codes) assert.equal(classify(code), 'fatal');
  }
  for (const waiting of [false, true]) {
    const { view, open, starts, pending } = rig();
    view.setForeground(true);
    view.start(open, true);
    const id = starts[0];
    view.playing(id);
    if (waiting) view.failed(id, 'disconnect');
    const timer = [...pending.values()][0];
    assert.equal(view.failed(id), 0); // Decoder, invalid multipart, unsupported URL etc. default to fatal.
    timer?.task();
    assert.equal(view.current(id), false);
    assert.equal(view.failed(id, 'disconnect'), 0);
    assert.equal(pending.size, 0);
    assert.equal(starts.length, 1);
  }
});
