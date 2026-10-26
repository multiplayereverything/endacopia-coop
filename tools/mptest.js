// Endacopia Multiplayer - unattended multi-instance playtest harness.
// Launches a host and up to three guests of the modded engine on this PC and drives
// them through the --mp-control automation port (no real mouse/keyboard use).
//
// usage: node mptest.js <scenario.txt> [--players N] [--keep] [--solo] [--show]
//
// Scenario commands (one per line, # comments):
//   start [host-args...]       launch the host and the first N-1 guests (only the host with --solo)
//   join <guest>               launch one more guest mid-session (guest2 / guest3)
//   leave <guest>              that guest quits
//   wait <ms>                  sleep
//   waitsync [timeout_ms]      until every running guest reports synced=1
//   <peer> <ctl command>       send an automation command to host / guest / guest2 / guest3
//   all <ctl command>          send to every running peer
//   shot <name>                screenshots of every peer -> out/<name>-<peer>.png
//   monkey <seconds> [seed]    random clicks/moves/keys from every player
//   check                      print state of every peer, fail if any guest reported desyncs
//   stop                       quit everyone
const net = require('net');
const fs = require('fs');
const path = require('path');
const { spawn, execFileSync } = require('child_process');

const MOD_DIR = path.resolve(__dirname, '..');
// hardlinked test copy of the install; never write here
const GAME_DIR = process.env.MP_GAME_DIR || path.join(MOD_DIR, 'game-test', 'Windows');
const ei = process.argv.indexOf('--engine');
const ENGINE = ei >= 0 ? path.resolve(process.argv[ei + 1]) : path.join(MOD_DIR, 'build', 'Release', 'ags.exe');
const LAB = path.join(process.env.LOCALAPPDATA, 'EndacopiaMP');
const OUT = path.join(MOD_DIR, 'tools', 'out');
const PORT = 27960;
const PEERS = ['host', 'guest', 'guest2', 'guest3'];
const CTL_PORT = { host: 28001, guest: 28002, guest2: 28003, guest3: 28004 };
const LAB_DIR = { host: 'lab-host', guest: 'lab-guest', guest2: 'lab-guest2', guest3: 'lab-guest3' };

const args = process.argv.slice(2);
const scenarioFile = args.find((a, i) => !a.startsWith('--') && !/^\d+$/.test(a) && args[i - 1] !== '--engine');
const KEEP = args.includes('--keep');
const SOLO = args.includes('--solo');
const SHOW = args.includes('--show');
const pi = args.indexOf('--players');
let PLAYERS = SOLO ? 1 : (pi >= 0 ? Math.max(1, Math.min(4, parseInt(args[pi + 1], 10))) : 2);

fs.mkdirSync(OUT, { recursive: true });
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().substr(11, 12), ...a);

class Ctl {
  constructor(name, port) { this.name = name; this.port = port; this.sock = null; this.buf = ''; this.waiters = []; }
  async connect(timeoutMs = 120000) {
    const until = Date.now() + timeoutMs;
    while (Date.now() < until) {
      try {
        await new Promise((res, rej) => {
          const s = net.connect(this.port, '127.0.0.1', () => { this.sock = s; res(); });
          s.on('error', rej);
          s.on('data', d => {
            this.buf += d.toString();
            let i;
            while ((i = this.buf.indexOf('\n')) >= 0) {
              const line = this.buf.slice(0, i); this.buf = this.buf.slice(i + 1);
              const w = this.waiters.shift(); if (w) w(line);
            }
          });
          s.on('close', () => { this.sock = null; this.waiters.splice(0).forEach(w => w('ERR closed')); });
        });
        return true;
      } catch (e) { await sleep(500); }
    }
    throw new Error(`${this.name}: control port ${this.port} not reachable`);
  }
  send(cmd, timeoutMs = 30000) {
    if (!this.sock) return Promise.resolve('ERR not connected');
    return new Promise(res => {
      const t = setTimeout(() => res('ERR timeout'), timeoutMs);
      this.waiters.push(line => { clearTimeout(t); res(line); });
      this.sock.write(cmd + '\n');
    });
  }
}

// Test windows are hidden so they don't cover the desktop; --show keeps them visible.
let hider = null;
function startHider() {
  if (SHOW || hider) return;
  const ps = "Add-Type -Namespace W -Name U -MemberDefinition '[DllImport(\"user32.dll\")] public static extern bool ShowWindow(System.IntPtr h, int n);';" +
    `while ($true) { Get-Process ags -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | ForEach-Object { [W.U]::ShowWindow($_.MainWindowHandle, 0) | Out-Null }; Start-Sleep -Milliseconds 200 }`;
  hider = spawn('powershell', ['-NoProfile', '-WindowStyle', 'Hidden', '-Command', ps], { stdio: 'ignore', windowsHide: true });
}

const procs = {};
const ctl = {};
for (const p of PEERS) ctl[p] = new Ctl(p, CTL_PORT[p]);
const running = () => PEERS.filter(p => procs[p] && procs[p].exitCode === null);
const runningGuests = () => running().filter(p => p !== 'host');

// Bad-network runs: the hub's chaos box in TCP mode sits between the host and every guest.
const PYTHON = process.env.MP_PYTHON || 'python';
const CHAOS_PY = path.join(MOD_DIR, 'coop-anything', 'tools', 'chaos', 'chaos.py');
const CHAOS_PORT = 27970, CHAOS_RPC = 27971;
let chaos = null;
// --mp-debug adds per-chunk hashes of script memory to every record; bandwidth tests turn it off ('nodebug') to
// send what players send.
let DEBUG = true;

async function startChaos(profile) {
  const logPath = path.join(OUT, 'chaos.jsonl');
  chaos = spawn(PYTHON, ['-I', CHAOS_PY, 'relay', '--tcp', '--target', `127.0.0.1:${PORT}`, '--listen', String(CHAOS_PORT),
    '--rpc-port', String(CHAOS_RPC), '--profile', profile, '--log', logPath], { stdio: ['ignore', 'pipe', 'inherit'], windowsHide: true });
  await new Promise((res, rej) => {
    chaos.stdout.once('data', d => { log('chaos relay up:', d.toString().trim()); res(); });
    chaos.once('exit', code => rej(new Error(`chaos relay exited (${code})`)));
  });
}

// Steam runs: a fake Steam (tools/steam-mock) gives every copy its own identity; guests join through lobbies.
const STEAM_MOCK_DLL = path.join(MOD_DIR, 'tools', 'steam-mock', 'build', 'Release', 'mock_steam_api.dll');
const STEAM_IDS = { host: '76561190000000001', guest: '76561190000000002', guest2: '76561190000000003', guest3: '76561190000000004' };
let steamBroker = null;

async function startSteamMock() {
  steamBroker = spawn(PYTHON, ['-I', path.join(MOD_DIR, 'tools', 'steam-mock', 'broker.py'), '--port', '27990'],
    { stdio: ['ignore', 'pipe', 'inherit'], windowsHide: true });
  await new Promise((res, rej) => {
    steamBroker.stdout.once('data', d => { log(d.toString().trim()); res(); });
    steamBroker.once('exit', code => rej(new Error(`steam mock broker exited (${code})`)));
  });
}

function chaosCall(method, params) {
  return new Promise(res => {
    const s = net.connect(CHAOS_RPC, '127.0.0.1', () => s.write(JSON.stringify({ jsonrpc: '2.0', id: 1, method, params }) + '\n'));
    s.once('data', d => { res(d.toString().trim()); s.end(); });
    s.on('error', e => res(`ERR ${e.message}`));
  });
}

function launch(role, extra) {
  startHider();
  const dir = path.join(LAB, LAB_DIR[role]);
  fs.mkdirSync(dir, { recursive: true });
  const logFile = path.join(dir, 'ags.log');
  try { fs.unlinkSync(logFile); } catch (e) {}
  const a = [path.join(GAME_DIR, 'Endacopia.exe'), '--windowed', '--gfxfilter', 'stdscale', '2',
    '--user-conf-dir', dir, '--log-file=all:info', `--log-file-path=${logFile}`, '--no-message-box', ...(DEBUG ? ['--mp-debug'] : []),
    '--mp-name', role, '--mp-control', String(CTL_PORT[role])];
  if (role === 'host') a.push('--user-data-dir', dir, '--mp-host', String(PORT));
  else a.push('--mp-join', steamBroker ? 'steam' : `127.0.0.1:${chaos ? CHAOS_PORT : PORT}`, '--mp-guest-dir', path.join(dir, 'saves'));
  if (steamBroker) a.push('--mp-steam', '--mp-steam-dll', STEAM_MOCK_DLL);
  a.push(...extra);
  if (process.env.MP_EXTRA) a.push(...process.env.MP_EXTRA.split(' '));
  // Guests run in other time zones (MSVC CRT reads TZ), like friends abroad would.
  const tz = { guest: 'GST-7', guest2: 'GST+5', guest3: 'GST-10' }[role];
  // Test copies play audio into SDL's silent dummy device: the game's audio clock still runs (the sync tests
  // check audio positions), nothing reaches the speakers, and the PC's own volume is never changed.
  const env = Object.assign({}, process.env, { SDL_AUDIODRIVER: 'dummy' }, tz ? { TZ: tz } : {},
    steamBroker ? { MOCK_STEAM_ID: STEAM_IDS[role], MOCK_STEAM_NAME: role, MOCK_STEAM_PORT: '27990' } : {});
  const p = spawn(ENGINE, a, { cwd: GAME_DIR, detached: false, stdio: 'ignore', env });
  p.on('exit', code => {
    log(`${role} exited (${code})`);
    if (!expectedExit[role]) problems.push(`${role}: exited unexpectedly (code ${code})`);
  });
  procs[role] = p;
  log(`launched ${role} pid ${p.pid}`);
}

async function launchGuest(role) {
  launch(role, []);
  await ctl[role].connect();
}

function stateField(s, k) { const m = new RegExp(`\\b${k}=(\\S+)`).exec(s); return m ? m[1] : null; }

async function shot(name) {
  for (const r of running()) {
    const bmp = path.join(OUT, `${name}-${r}.bmp`);
    try { fs.unlinkSync(bmp); } catch (e) {}
    await ctl[r].send(`shot ${bmp}`);
    await sleep(300);
    const png = bmp.replace(/\.bmp$/, '.png');
    try {
      execFileSync('powershell', ['-NoProfile', '-Command',
        `Add-Type -AssemblyName System.Drawing; $i=[System.Drawing.Image]::FromFile('${bmp}'); $i.Save('${png}',[System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item '${bmp}'`]);
      log(`shot ${png}`);
    } catch (e) { log(`shot ${r} failed: ${e.message}`); }
  }
}

function rng(seed) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296); }

async function monkey(seconds, seed) {
  const R = rng(seed || 12345);
  const until = Date.now() + seconds * 1000;
  let n = 0;
  while (Date.now() < until) {
    const peers = running().filter(p => !droppedSolo[p]);
    if (peers.length === 0) { log('!! monkey: no game left running'); return; }
    const who = peers[Math.floor(R() * peers.length)];
    const x = Math.floor(R() * 384), y = Math.floor(R() * 216);
    const r = R();
    if (r < 0.45) await ctl[who].send(`move ${x} ${y}`);
    else if (r < 0.85) await ctl[who].send(`click ${x} ${y} 1`);
    else if (r < 0.93) await ctl[who].send(`click ${x} ${y} 2`);
    else await ctl[who].send(`key ${[44, 40, 41, 79, 80, 81, 82][Math.floor(R() * 7)]}`); // space enter esc arrows
    n++;
    await sleep(80 + Math.floor(R() * 400));
    if (n % 40 === 0) {
      for (const g of runningGuests()) {
        const s = await ctl[g].send('state');
        log(`monkey ${n} ${g}: tick=${stateField(s, 'tick')} synced=${stateField(s, 'synced')} desyncs=${stateField(s, 'desyncs')} room=${stateField(s, 'room')}`);
      }
    }
  }
}

// Scoring is one pure function, so --selftest can feed it known-bad input and check it says FAIL.
// It returns the list of problems in one peer's "state" reply; an empty list means that peer is fine.
// Progress between two checks of one guest: it must keep moving, and not fall more than 30 s behind the host.
// A guest that dropped to solo (phase 4) is acceptable only where the scenario allows it ('allow-drop').
function scoreProgress(peer, prevTick, tick, hostTick, phase, allowDrop) {
  if (phase === '4') return allowDrop ? [] : [`${peer}: dropped to solo play`];
  const problems = [];
  if (prevTick !== null && tick <= prevTick) problems.push(`${peer}: frozen at frame ${tick}`);
  if (hostTick !== null && hostTick - tick > 1200) problems.push(`${peer}: ${hostTick - tick} frames (${((hostTick - tick) / 40).toFixed(0)} s) behind the host`);
  return problems;
}

function scoreState(peer, reply, guestsExpected) {
  if (process.env.COOP_MUT === 'scorer_always_ok') return []; // twin: a scorer that never fails
  const problems = [];
  if (!reply || !reply.startsWith('OK')) return [`${peer}: no valid state reply (${reply || 'nothing'}) - crashed or hung`];
  const synced = stateField(reply, 'synced');
  const desyncs = parseInt(stateField(reply, 'desyncs') || 'NaN', 10);
  if (peer === 'host') {
    if (guestsExpected > 0 && synced !== '1') problems.push('host: not in co-op (fell back to solo)');
  } else {
    if (synced !== '1') problems.push(`${peer}: not in co-op (synced=${synced})`);
    if (Number.isNaN(desyncs)) problems.push(`${peer}: state reply has no desync counter`);
    else if (desyncs > 0) problems.push(`${peer}: ${desyncs} desync(s); see ${LAB_DIR[peer]}/ags.log ([MP] lines)`);
  }
  return problems;
}

const problems = [];
let checksRun = 0;
let ALLOW_DROP = false;
const lastTick = {};
const droppedSolo = {};  // guests that left the session cleanly (allow-drop); random input skips them
const expectedExit = {};

async function check() {
  checksRun++;
  const guests = runningGuests().length;
  let ok = true;
  let hostTick = null;
  for (const r of PEERS.filter(p => procs[p] && !expectedExit[p])) {
    const s = procs[r].exitCode === null ? await ctl[r].send('state') : `exited with code ${procs[r].exitCode}`;
    log(r.toUpperCase().padEnd(6), s);
    const tick = parseInt(stateField(s, 'tick') || 'NaN', 10);
    if (r === 'host') hostTick = Number.isNaN(tick) ? null : tick;
    let found = [];
    if (r !== 'host' && ALLOW_DROP && stateField(s, 'phase') === '4') {
      found = scoreProgress(r, lastTick[r] ?? null, tick, null, '4', true);
      if (found.length === 0) { log(`   ${r} dropped to solo play (allowed in this scenario)`); droppedSolo[r] = true; }
    } else {
      // with a dropped guest the host is legitimately solo too
      const guestsNow = ALLOW_DROP && runningGuests().length > 0 ? 0 : guests;
      found = scoreState(r, s, guestsNow);
      if (r !== 'host' && found.length === 0)
        found = scoreProgress(r, lastTick[r] ?? null, tick, hostTick, stateField(s, 'phase'), ALLOW_DROP);
    }
    if (!Number.isNaN(tick)) lastTick[r] = tick;
    for (const p of found) { log('!!', p); problems.push(p); ok = false; }
  }
  return ok;
}

async function quitAndWait(role) {
  expectedExit[role] = true;
  await ctl[role].send('quit', 3000);
  const until = Date.now() + 10000;
  while (procs[role].exitCode === null && Date.now() < until) await sleep(200);
  if (procs[role].exitCode === null) problems.push(`${role}: did not close when asked to quit`);
}

function selfTest() {
  const good = 'OK tick=10 role=2 synced=1 phase=2 records=0 desyncs=0 hash=0 room=1';
  const cases = [
    ['guest', good, 1, true],
    ['guest', good.replace('desyncs=0', 'desyncs=3'), 1, false],
    ['guest', good.replace('synced=1', 'synced=0'), 1, false],
    ['guest', 'ERR timeout', 1, false],
    ['guest', '', 1, false],
    ['guest', 'exited with code 3221225477', 1, false],
    ['guest', good.replace(' desyncs=0', ''), 1, false],
    ['host', good.replace('role=2', 'role=1').replace('synced=1', 'synced=0'), 1, false],
    ['host', good.replace('role=2', 'role=1').replace('synced=1', 'synced=0'), 0, true],
  ];
  const progress = [
    [100, 500, 520, '2', false, true],
    [500, 500, 900, '2', false, false],      // frozen
    [null, 100, 2000, '2', false, false],    // 47 s behind
    [100, 300, 400, '4', false, false],      // dropped, not allowed
    [100, 300, 400, '4', true, true],        // dropped, allowed
  ];
  let bad = 0;
  for (const [prev, tick, host, phase, allow, expectOk] of progress) {
    const ok = process.env.COOP_MUT === 'progress_always_ok' ? true : scoreProgress('guest', prev, tick, host, phase, allow).length === 0;
    if (ok !== expectOk) { bad++; log(`selftest: progress ${prev}->${tick} host ${host} phase ${phase} gave ${ok ? 'PASS' : 'FAIL'}`); }
  }
  for (const [peer, reply, guests, expectOk] of cases) {
    const ok = scoreState(peer, reply, guests).length === 0;
    if (ok !== expectOk) { bad++; log(`selftest: scoring "${reply}" for ${peer} gave ${ok ? 'PASS' : 'FAIL'}, expected ${expectOk ? 'PASS' : 'FAIL'}`); }
  }
  return bad === 0;
}

async function run() {
  const lines = fs.readFileSync(scenarioFile, 'utf8').split(/\r?\n/);
  let ok = true;
  for (let raw of lines) {
    // %NAME% expands from the environment, so scenarios can say %LOCALAPPDATA%
    const line = raw.replace(/#.*/, '').trim().replace(/%(\w+)%/g, (m, k) => process.env[k] ?? m);
    if (!line) continue;
    const [cmd, ...rest] = (line.match(/"[^"]*"|\S+/g) || []).map(t => t.replace(/^"(.*)"$/, '$1'));
    log('>', line);
    switch (cmd) {
      case 'start':
        launch('host', rest);
        await ctl.host.connect();
        for (let i = 1; i < PLAYERS; i++) { await sleep(1500); await launchGuest(PEERS[i]); }
        break;
      case 'players': PLAYERS = parseInt(rest[0], 10); break;  // before start
      case 'nodebug': DEBUG = false; break;  // before start
      case 'allow-drop': ALLOW_DROP = true; break;  // a clean drop to solo is an acceptable outcome
      case 'join': await launchGuest(rest[0]); break;
      case 'leave': await quitAndWait(rest[0]); break;
      case 'wait': await sleep(parseInt(rest[0], 10)); break;
      case 'waitsync': {
        const until = Date.now() + parseInt(rest[0] || '180000', 10);
        for (const g of runningGuests()) {
          let s = '';
          while (Date.now() < until) {
          s = await ctl[g].send('state');
          if (stateField(s, 'synced') === '1' || (ALLOW_DROP && stateField(s, 'phase') === '4')) break;
          await sleep(500);
        }
          log(`${g}:`, s);
          if (ALLOW_DROP && stateField(s, 'phase') === '4') { log(`   ${g} could not join and plays solo (allowed in this scenario)`); droppedSolo[g] = true; continue; }
          if (stateField(s, 'synced') !== '1') { log(`!! ${g} never synced`); ok = false; }
        }
        break;
      }
      case 'host': case 'guest': case 'guest2': case 'guest3': log(cmd, await ctl[cmd].send(rest.join(' '))); break;
      case 'all': case 'both':
        for (const r of running()) log(r, await ctl[r].send(rest.join(' ')));
        break;
      case 'steam': await startSteamMock(); break;  // before start: join through (fake) Steam lobbies
      case 'chaos': await startChaos(rest[0] || 'lan'); break;  // before start: guests join through it
      case 'chaos-set': log('chaos', await chaosCall('set_profile', { profile: rest[0] })); break;
      case 'chaos-partition': log('chaos', await chaosCall('partition', { dir: 'both', on: true, duration_ms: parseInt(rest[0], 10) })); break;
      case 'chaos-stats': log('chaos', await chaosCall('stats', {})); break;
      case 'shot': await shot(rest[0] || 'shot'); break;
      case 'monkey': await monkey(parseFloat(rest[0] || '30'), parseInt(rest[1] || '1', 10)); break;
      case 'check': if (!(await check())) ok = false; break;
      case 'stop': for (const r of running().reverse()) await quitAndWait(r); break;
      default: log('unknown scenario command', cmd);
    }
  }
  return ok;
}

function cleanup() {
  if (!KEEP) for (const p of Object.values(procs)) { try { process.kill(p.pid); } catch (e) {} }
  if (hider) try { process.kill(hider.pid); } catch (e) {}
  if (chaos) try { process.kill(chaos.pid); } catch (e) {}
  if (steamBroker) try { process.kill(steamBroker.pid); } catch (e) {}
}

if (args.includes('--selftest')) {
  const ok = selfTest();
  console.log(`RESULT: ${ok ? 'PASS' : 'FAIL'} (scorer self-test)`);
  process.exit(ok ? 0 : 1);
}

run().then(ok => {
  // A scenario that never checked state proves nothing.
  if (checksRun === 0) problems.push('no check ran: hollow pass');
  if (!ok && problems.length === 0) problems.push('a scenario step failed (see log above)');
  cleanup();
  for (const p of problems) log('!!', p);
  console.log(problems.length === 0 ? 'RESULT: PASS' : `RESULT: FAIL (${problems.length} problem(s))`);
  process.exit(problems.length === 0 ? 0 : 1);
}).catch(e => {
  log('harness error', e.message);
  cleanup();
  console.log(`RESULT: FAIL (harness error: ${e.message})`);
  process.exit(2);
});
