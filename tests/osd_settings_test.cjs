// node --test tests/osd_settings_test.cjs (Node 22.18+)
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
const source = readFileSync(join(__dirname, '../entry/src/main/ets/util/OsdSettings.ets'), 'utf8')
  .replace("import { batteryInfo } from '@kit.BasicServicesKit';", '').replace(/^export /gm, '');
const { osdRestartRequired, osdChargeSource } = new Function('batteryInfo', stripTypeScriptTypes(source) +
  '\nreturn { osdRestartRequired, osdChargeSource };')({
    BatteryChargeState: { NONE: 0, ENABLE: 1, DISABLE: 2, FULL: 3 },
    BatteryPluggedType: { NONE: 0, AC: 1, USB: 2, WIRELESS: 3 }
  });

test('all 16 transitions restart only on crossing the direct/composited boundary', () => {
  for (let old = 0; old < 4; ++old) for (let next = 0; next < 4; ++next) {
    assert.equal(osdRestartRequired(!!(old & 1), !!(old & 2), !!(next & 1), !!(next & 2)),
      (old === 0) !== (next === 0), `${old} -> ${next}`);
  }
});
test('charge status is not a charger type; only active AC/USB/wireless has a suffix', () => {
  for (const source of [0, 1, 2, 3, 99]) {
    for (const status of [0, 2, 3, 99]) assert.equal(osdChargeSource(status, source), 0);
    assert.equal(osdChargeSource(1, source), source <= 3 ? source : 0);
  }
});
