"""Deterministic host regressions for motion state and recording control.

Run: python tests/motion_state_test.py
Requires Node (NODE may override) and C++17 (CXX, or MSVC/VCVARS64 on Windows).
Only temporary files are generated. Native methods/header/constants are extracted
from production; only NowUs and UpdateWorker are stubbed. ArkTS method bodies are
executed by Node after erasing the few type annotations used by these methods.
Fake timers, filesystem, recorder, and status delivery replace platform services.
This does not exercise the codec, actual N-API queue, worker join, or ArkTS runtime;
queue loss means withholding all notifications, and queued callbacks are invoked
explicitly. Lifecycle/API checks below are structural, not runtime teardown tests.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "entry/src/main/cpp"
ETS = ROOT / "entry/src/main/ets"
TOKENS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|`(?:\\.|[^`\\])*`|[{}]', re.S)


def definition(source, marker):
    """Extract one balanced definition, ignoring comments and quoted text."""
    if source.count(marker) != 1:
        raise AssertionError(f"Expected exactly one source marker: {marker!r}")
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 0
    for token in TOKENS.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:token.end()]
    raise AssertionError(f"Unclosed definition: {marker}")


def run(command, cwd, timeout=30):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=timeout)
    if result.returncode:
        raise AssertionError(f"Command failed ({result.returncode}): {command}\n"
                             f"{result.stdout}\n{result.stderr}")
    return result.stdout


def compile_native(source, directory):
    exe = directory / ("motion.exe" if os.name == "nt" else "motion")
    if os.name == "nt" and not os.environ.get("CXX"):
        vcvars = Path(os.environ.get("VCVARS64", "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat"))
        setup = ""
        if not shutil.which("cl"):
            if not vcvars.is_file():
                raise RuntimeError("Install C++17 compiler or set CXX/VCVARS64")
            setup = f'@call "{vcvars}" >nul\n@if errorlevel 1 exit /b %errorlevel%\n'
        build = directory / "build.cmd"
        build.write_text(setup + f'@cl /nologo /std:c++17 /EHsc /W4 /utf-8 "{source}" /Fe:"{exe}" /Fo:"{directory / "motion.obj"}"\n', encoding="utf-8")
        run(["cmd.exe", "/d", "/c", str(build)], directory, timeout=120)
    else:
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("Install a C++17 compiler or set CXX")
        run([compiler, "-std=c++17", "-pthread", str(source), "-o", str(exe)], directory, timeout=120)
    return exe


NATIVE_TESTS = r'''
using ipcam::WebStream;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void sample(WebStream& stream, uint8_t level, uint64_t generation) {
  std::vector<uint8_t> frame(128 * 72 * 3 / 2, level);
  stream.CheckMotion(frame.data(), 128, 72, generation);
}
void sample(WebStream& stream, uint8_t level) {
  sample(stream, level, stream.motionGeneration_.load());
}
void hit(WebStream& stream) {
  stream.SetMotionEnabled(true);
  ipcam::fakeNow = static_cast<uint64_t>(stream.armAtUs_.load());
  sample(stream, 0);
  require(!stream.MotionActive(), "baseline is not a hit");
  ++ipcam::fakeNow;
  sample(stream, 255);
  require(stream.MotionActive(), "armed changed frame must be active");
}
int main() {
  try {
    {
      ipcam::fakeNow = 0;
      WebStream s;
      require(!s.MotionActive(), "initially disabled");
      sample(s, 255);
      require(s.prevGrid_.empty(), "disabled samples cannot change baseline");
      s.SetMotionEnabled(true);
      sample(s, 0);
      ++ipcam::fakeNow;
      sample(s, 255);
      require(!s.MotionActive() && s.lastMotionUs_ == 0, "unarmed hit ignored");
      ipcam::fakeNow = static_cast<uint64_t>(s.armAtUs_.load());
      s.UpdateMotionLatch();
      require(!s.MotionActive() && !s.motionLatch_, "arming without hit stays idle near boot");
      sample(s, 255);
      require(!s.MotionActive(), "unchanged frame stays idle");
    }
    std::cout << "PASS native disabled/no-hit/arming\n";
    {
      ipcam::fakeNow = 100000000;
      WebStream s;
      std::vector<bool> events;
      s.SetMotionSink([&](bool active) {
        events.push_back(active);
        (void)s.MotionActive();  // Reentrant query must not deadlock on motionMu_.
      });
      hit(s);
      const uint64_t last = static_cast<uint64_t>(s.lastMotionUs_.load());
      ipcam::fakeNow = last + 15000000 - 1;
      require(s.MotionActive(), "active immediately before expiry");
      ipcam::fakeNow++;
      require(!s.MotionActive(), "query expires without worker/latch progress");
      require(s.motionLatch_ && events == std::vector<bool>{true}, "no clear event was delivered");
      s.UpdateMotionLatch();
      require(events == std::vector<bool>({true, false}), "advisory latch eventually catches up");
    }
    std::cout << "PASS native timeout independent of worker and notifications\n";
    {
      ipcam::fakeNow = 200000000;
      WebStream s;
      hit(s);
      const uint64_t old = s.motionGeneration_.load();
      s.SetMotionEnabled(false);
      require(!s.MotionActive() && !s.motionLatch_ && !s.motionDemand_, "disable resets state");
      sample(s, 0, old);
      require(s.prevGrid_.empty() && s.lastMotionUs_ == 0, "disabled old sample ignored");
      s.SetMotionEnabled(true);
      ipcam::fakeNow = static_cast<uint64_t>(s.armAtUs_.load());
      sample(s, 255, old);  // Old decoded frame completes in the new session.
      require(s.prevGrid_.empty() && s.lastMotionUs_ == 0, "stale generation cannot seed baseline");
      sample(s, 0);
      sample(s, 255, old);
      require(!s.MotionActive() && s.prevGrid_.front() == 0, "old generation cannot trigger a hit");
      sample(s, 255);
      require(s.MotionActive(), "new generation still detects motion");
    }
    std::cout << "PASS native disable/re-enable rejects old generation\n";
    {
      ipcam::fakeNow = 300000000;
      WebStream s;
      hit(s);
      const uint64_t last = static_cast<uint64_t>(s.lastMotionUs_.load());
      s.SetMotionTimeout(-1);
      require(s.motionHoldUs_ == 10000000, "minimum timeout clamp");
      ipcam::fakeNow = last + 10000000;
      require(!s.MotionActive(), "updated minimum takes effect in getter");
      s.SetMotionTimeout(999);
      require(s.motionHoldUs_ == 120000000, "maximum timeout clamp");
      ipcam::fakeNow = last + 120000000 - 1;
      require(s.MotionActive(), "updated maximum takes effect in getter");
      ++ipcam::fakeNow;
      require(!s.MotionActive(), "exact maximum expiry");
      ipcam::fakeNow = last - 1;
      require(!s.MotionActive(), "future timestamp cannot underflow into active");
    }
    std::cout << "PASS native timeout update/boundaries\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
'''

UI_FIXTURE = r'''
const assert = require('node:assert/strict');
let nextTimer = 0;
const timers = new Map();
function setInterval(callback, delay) {
  assert.equal(delay, 250);
  const id = ++nextTimer;
  timers.set(id, callback);
  return id;
}
function clearInterval(id) { timers.delete(id); }
const fileIo = { accessSync: () => true, mkdirSync: () => {} };
function getContext() { return { filesDir: '/test' }; }
const RtspEvent = { Connected: 0, Disconnected: 1, Playing: 2, Error: 3 };
const RtmpState = { Idle: 0 };
class Page {
  constructor() {
    Object.assign(this, { running: true, cameraRunning: true, pageActive: true,
      motionEnabled: false, motionRecording: false, recording: false,
      motionTimerId: -1, motionGeneration: 0, motionRecordPath: '',
      motionTimeout: '15', segMinutes: '10', segFourGb: true, logs: [] });
    this.server = {
      enabled: false, active: false, running: true, starts: 0, stops: 0, queries: 0,
      motionActive() { this.queries++; return this.running && this.enabled && this.active; },
      setMotion(enabled) { this.enabled = enabled; this.active = false; },
      startRecord(path) { assert.match(path, /\/MD\/IPS_.*\.mp4$/); this.starts++; return true; },
      stopRecord() { this.stops++; if (this.failStop) throw new Error('stop failed'); }
    };
  }
  appendLog(message) { this.logs.push(message); }
  motionTimestamp() { return 'test'; }
  saveSetting() {}
  startCameraNow() { return true; }
  onStartServer() { this.running = true; }
__METHODS__
}
function tick(page) { timers.get(page.motionTimerId)(); }
function activePage() {
  const page = new Page();
  page.onToggleMotion();
  page.server.active = true;
  tick(page);
  assert.equal(page.server.starts, 1);
  return page;
}
function test(name, body) { timers.clear(); body(); console.log('PASS UI ' + name); }
'''

UI_TESTS = r'''
test('lost all status including stop; idempotent polling', () => {
  const p = activePage();  // No status callback has been delivered at all.
  tick(p);
  assert.equal(p.server.starts, 1);
  p.server.active = false;
  tick(p); tick(p);
  assert.equal(p.server.stops, 1);
  assert.equal(p.motionRecording, false);
  assert.equal(p.motionRecordPath, '');
});
test('delayed detected/cleared callbacks are log-only', () => {
  const p = new Page();
  p.onToggleMotion();
  p.onStatus(2, 0, 'MOTION detected');
  assert.equal(p.server.starts, 0);
  p.server.active = true;
  tick(p);
  p.onStatus(2, 0, 'MOTION cleared');
  assert.equal(p.server.stops, 0);
  p.onToggleMotion();
  p.onStatus(2, 0, 'MOTION detected');
  p.onStatus(2, 0, 'MOTION cleared');
  assert.equal(p.server.starts, 1);
  assert.equal(p.server.stops, 1);
  assert.equal(p.server.enabled, false);
  assert.equal(p.motionTimerId, -1);
  assert.equal(p.motionRecordPath, '');
  assert.equal(timers.size, 0);
});
test('fast re-enable rejects queued old timers and old status', () => {
  const p = activePage();
  const oldTick = timers.get(p.motionTimerId);
  p.onToggleMotion();
  p.onToggleMotion();
  const queries = p.server.queries;
  oldTick();
  assert.equal(p.server.queries, queries, 'old timer must not even query');
  p.onStatus(2, 0, 'MOTION detected');
  assert.equal(p.server.starts, 1);
  p.server.active = true;
  tick(p);
  p.onStatus(2, 0, 'MOTION cleared');
  assert.equal(p.server.stops, 1);
  p.server.active = false;
  oldTick();
  assert.equal(p.server.stops, 1, 'old timer cannot close the new recording');
  tick(p);
  assert.equal(p.server.starts, 2);
  assert.equal(p.server.stops, 2);
});
test('timer replacement leaves one current timer', () => {
  const p = new Page();
  p.onToggleMotion();
  const oldTick = timers.get(p.motionTimerId);
  p.startMotionTimer();
  assert.equal(timers.size, 1);
  p.server.active = true;
  oldTick();
  assert.equal(p.server.starts, 0);
  tick(p);
  assert.equal(p.server.starts, 1);
});
test('start guards and stopped native server', () => {
  const p = new Page();
  p.onToggleMotion();
  p.server.active = true;
  p.pageActive = false;
  tick(p);
  assert.equal(p.server.starts, 0);
  p.pageActive = true;
  p.recording = true;
  tick(p);
  assert.equal(p.server.starts, 0, 'manual recorder is not replaced');
  p.recording = false;
  tick(p);
  p.server.running = false;
  tick(p);
  assert.equal(p.server.stops, 1);
});
test('native disable still runs when recorder stop throws', () => {
  const p = activePage();
  p.server.failStop = true;
  assert.throws(() => p.stopMotionMode('off'), /stop failed/);
  assert.equal(p.motionEnabled, false);
  assert.equal(p.server.enabled, false);
  assert.equal(p.motionTimerId, -1);
  assert.equal(timers.size, 0);
});
'''


class MotionRegression(unittest.TestCase):
    def test_native_state_from_production_methods(self):
        source = (CPP / "web_stream.cpp").read_text(encoding="utf-8")
        header = (CPP / "web_stream.h").read_text(encoding="utf-8")
        header = re.sub(r"(?m)^\s*private:", " public:", header)
        methods = ["WebStream::~WebStream()", "void WebStream::SetMotionTimeout(",
                   "bool WebStream::MotionActive()", "void WebStream::SetMotionEnabled(",
                   "void WebStream::SetMotionSink(", "bool WebStream::MotionArmed(",
                   "void WebStream::UpdateMotionLatch()", "void WebStream::CheckMotion("]
        constants = "\n".join(re.findall(r"^constexpr (?:int|int64_t) k(?:GridW|GridH|CellDelta|ChangedCells|ArmDelayUs|TimeoutMinSeconds|TimeoutMaxSeconds)\b[^;]*;", source, re.M))
        unit = ('#include <algorithm>\n#include <iostream>\n#include <stdexcept>\n' + header +
                '\nnamespace ipcam {\nnamespace {\nuint64_t fakeNow = 0;\n'
                'uint64_t NowUs() { return fakeNow; }\n' + constants + '\n}\n' +
                '\n'.join(definition(source, marker) for marker in methods) +
                '\nvoid WebStream::UpdateWorker() { (void)MotionActive(); }\n}\n' + NATIVE_TESTS)
        with tempfile.TemporaryDirectory(prefix="motion-test-") as temp:
            directory = Path(temp)
            cpp = directory / "motion.cpp"
            cpp.write_text(unit, encoding="utf-8")
            exe = compile_native(cpp, directory)
            print(run([str(exe)], directory, timeout=15), end="")

    def test_ui_control_from_production_methods(self):
        source = (ETS / "pages/Index.ets").read_text(encoding="utf-8")
        names = ["onToggleMotion", "startMotionTimer", "stopMotionTimer",
                 "onMotionStart", "onMotionStop", "stopMotionMode"]
        methods = "\n".join(definition(source, f"\n  {name}(") for name in names)
        callback = definition(source, "(event: number, clients: number, detail: string) => {")
        methods += "\n  onStatus(event, clients, detail) " + callback.split("=>", 1)[1]
        methods = re.sub(r":\s*(?:void|boolean|number|string|RecordOptions)\b", "", methods)
        methods = methods.replace(" as common.UIAbilityContext", "")
        deveco = Path("C:/Program Files/Huawei/DevEco Studio/tools/node/node.exe")
        node = os.environ.get("NODE") or (str(deveco) if deveco.is_file() else shutil.which("node"))
        if not node:
            self.fail("Install Node or set NODE to its executable")
        with tempfile.TemporaryDirectory(prefix="motion-ui-test-") as temp:
            js = Path(temp) / "motion.cjs"
            js.write_text(UI_FIXTURE.replace("__METHODS__", methods) + UI_TESTS, encoding="utf-8")
            print(run([node, str(js)], Path(temp)), end="")

    def test_api_and_lifecycle_wiring(self):
        napi = (CPP / "napi_init.cpp").read_text(encoding="utf-8")
        query = definition(napi, "napi_value RtspMotionActive(")
        self.assertIn("holder->server.IsRunning() && holder->webStream->MotionActive()", query)
        self.assertRegex(napi, r'\{"rtspMotionActive",\s*nullptr,\s*RtspMotionActive,')
        types = (CPP / "types/libstreaming/index.d.ts").read_text(encoding="utf-8")
        self.assertRegex(types, r"rtspMotionActive:\s*\(server: object\)\s*=>\s*boolean")
        model = (ETS / "model/RtspServer.ets").read_text(encoding="utf-8")
        self.assertIn("return rtspMotionActive(this.handle)", definition(model, "\n  motionActive("))
        post = definition(napi, "void PostEvent(ServerHolder* holder, ipcam::RtspEvent event, int clients,\n               const std::string& detail) {")
        self.assertIn("napi_tsfn_nonblocking", post)
        self.assertIn("delete ev", post)
        finalizer = definition(napi, "void ServerFinalize(")
        order = [finalizer.index(marker) for marker in (
            "webStream->DecClients()", "webStream->SetMotionEnabled(false)",
            "webStream->SetMotionSink(nullptr)", "napi_release_threadsafe_function", "delete holder")]
        self.assertEqual(order, sorted(order))
        web = (CPP / "web_stream.cpp").read_text(encoding="utf-8")
        decode = definition(web, "void WebStream::DecodeLoop()")
        self.assertLess(decode.index("motionGeneration_.load()"), decode.index("DecodeOneFrame("))
        self.assertRegex(decode, r"CheckMotion\([^;]*motionGeneration\)")
        page = (ETS / "pages/Index.ets").read_text(encoding="utf-8")
        for name in ("onStopServer", "onStopCamera", "aboutToDisappear"):
            self.assertIn("this.stopMotionMode(", definition(page, f"\n  {name}("))


if __name__ == "__main__":
    unittest.main(verbosity=2)
