// DevEco resolves extensionless .ts imports; Node's ESM loader requires .ts.
// Resolve just the two local imports under test, without changing application code.
const { registerHooks } = require('node:module');
const { pathToFileURL } = require('node:url');
const { resolve } = require('node:path');
const base = pathToFileURL(resolve(__dirname, '../entry/src/main/ets/util') + '/').href;
const hook = registerHooks({ resolve(specifier, context, next) {
  if (context.parentURL?.startsWith(base) && ['./RtmpProfiles', './RtmpTlsIdentity'].includes(specifier)) {
    return next(specifier + '.ts', context);
  }
  return next(specifier, context);
} });
try {
  module.exports = { ...require('../entry/src/main/ets/util/RtmpProfiles.ts'),
    ...require('../entry/src/main/ets/util/RtmpRetryController.ts') };
} finally {
  hook.deregister();
}
