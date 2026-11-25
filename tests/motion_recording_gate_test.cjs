// node --test tests/motion_recording_gate_test.cjs (Node >= 22.18)
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { MotionRecordingGate } = require('../entry/src/main/ets/util/MotionRecordingGate.ts');

test('mkdir/open/reservation failure or a stopped writer cannot reopen on repeated active samples', () => {
  const gate = new MotionRecordingGate();
  assert.equal(gate.claim(true), true);
  for (let tick = 0; tick < 1000; ++tick) assert.equal(gate.claim(true), false);
  gate.rearm(); // observed quiet, not just a writer error
  assert.equal(gate.claim(true), true);
  assert.equal(gate.claim(true), false);
});

test('a hidden hit is not deferred until foreground; a fresh episode can start there', () => {
  const gate = new MotionRecordingGate();
  assert.equal(gate.claim(false), false);
  assert.equal(gate.claim(true), false);
  gate.rearm();
  assert.equal(gate.claim(true), true);
  assert.equal(gate.claim(false), false);
  assert.equal(gate.claim(true), false);
});

test('explicitly arming a new session clears the previous failure; quiet samples may repeat', () => {
  const gate = new MotionRecordingGate();
  assert.equal(gate.claim(true), true);
  gate.rearm();
  gate.rearm();
  assert.equal(gate.claim(true), true);
  assert.equal(gate.claim(true), false);
});
