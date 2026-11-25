// Execute the actual ArkTS helpers with Node's TypeScript stripper (no device).
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { stripTypeScriptTypes } = require('node:module');
const { readFileSync } = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const src = readFileSync(path.join(__dirname, '../entry/src/main/ets/model/CameraCapabilities.ets'), 'utf8');
const { parseCameraDevices, parseEncoderOptions, parseFocusInfo, parseCodecTuning, codecTuningKey,
  resolveCameraBitrate } = vm.runInNewContext(stripTypeScriptTypes(src.replaceAll('export ', '') +
  '\n;({parseCameraDevices,parseEncoderOptions,parseFocusInfo,parseCodecTuning,codecTuningKey,resolveCameraBitrate})'));
const plain = x => JSON.parse(JSON.stringify(x));

test('offer only reported device IDs, implementations, focus modes and profiles', () => {
  assert.deepEqual(plain(parseCameraDevices('[{"id":"x","front":false},{"id":"x","front":true},{"id":"","front":false}]')),
    [{ id: 'x', front: false }]);
  const options = [{ name: 'cpu.avc', hardware: false, profiles: [8, 8, 5, -1], minBitrate: 100, maxBitrate: 5000000 }];
  assert.deepEqual(plain(parseEncoderOptions(JSON.stringify([...options, ...options, {}, null]))),
    [{ ...options[0], profiles: [8] }]);
  assert.deepEqual(plain(parseEncoderOptions('bad json')), []);
  assert.deepEqual(plain(parseFocusInfo('{"modes":[2,9,2,0],"active":0}')), { modes: [2,0], active: 0 });
  assert.deepEqual(plain(parseFocusInfo('{"modes":[],"active":44}')), { modes: [] });
});

test('separate AVC/HEVC tuning values, migrate former settings, reject invalid persisted numbers', () => {
  const prefs = new Map();
  const avc = codecTuningKey('video/avc'), hevc = codecTuningKey('video/hevc');
  assert.notEqual(avc, hevc);
  prefs.set(avc, JSON.stringify({ bitrate: 3890000, iFrameMs: 3500 }));
  assert.deepEqual(plain(parseCodecTuning(prefs.get(hevc) ?? '', 2000000, 1000)), { bitrate: 2000000, iFrameMs: 1000 });
  prefs.set(hevc, JSON.stringify({ bitrate: 0, iFrameMs: 1500 }));
  assert.deepEqual(plain(parseCodecTuning(prefs.get(avc), 1, 1)), { bitrate: 3890000, iFrameMs: 3500 });
  assert.deepEqual(plain(parseCodecTuning(prefs.get(hevc), 1, 1)), { bitrate: 0, iFrameMs: 1500 });
  assert.deepEqual(plain(parseCodecTuning('{"bitrate":2147483648,"iFrameMs":1.5}', 0, 0)), { bitrate: 0, iFrameMs: 2000 });
});

test('initial auto targets follow codec, clamp only auto, use precise integer bit/s', () => {
  const auto = { bitrate: 0, iFrameMs: 2000 };
  const soft = { name: 'soft', hardware: false, profiles: [], minBitrate: 0, maxBitrate: 0 };
  assert.equal(resolveCameraBitrate(auto, 640, 480), 640 * 480 * 6);
  assert.equal(resolveCameraBitrate(auto, 640, 480, soft), 640 * 480 * 4);
  assert.equal(resolveCameraBitrate(auto, 640, 480, soft, 'video/hevc'), 640 * 480 * 2);
  const bounded = { ...soft, minBitrate: 700000, maxBitrate: 800000 };
  assert.equal(resolveCameraBitrate(auto, 640, 480, bounded, 'video/hevc'), 700000);
  assert.equal(resolveCameraBitrate(auto, 1920, 1080, bounded), 800000);
  assert.equal(resolveCameraBitrate({ bitrate: 1200000, iFrameMs: 1234 }, 1920, 1080, bounded), 1200000);
  assert.equal(resolveCameraBitrate(auto, -1, 1080, bounded), 0);
});
