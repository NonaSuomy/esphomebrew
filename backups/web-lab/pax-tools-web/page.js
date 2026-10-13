'use strict';
const $ = id => document.getElementById(id);
let active = null, pending = null, currentApp = '', saveTimer = null, loading = false;
const modules = {};
const returns = [];
let emulatedHost = null, emulatedCPU = null, sharedAudio = null;
const log = message => {
  if (/^(INITIAL$|BEFORE_|IN_|AFTER_|GENERIC_)/.test(message))
    return;
  const box = $('log');
  box.textContent = (box.textContent + '\n' + message).slice(-30000);
  box.scrollTop = box.scrollHeight;
};
const status = message => { $('status').textContent = message; };
function loadScript(name) {
  return new Promise((resolve, reject) => {
    if (modules[name])
      return resolve();
    const script = document.createElement('script');
    script.src = name + '.js';
    script.onload = () => {
      modules[name] = window.createPax;
      resolve();
    };
    script.onerror = () => reject(new Error(`Missing ${name}.js build`));
    document.body.append(script);
  });
}
function sync(module, populate) {
  return new Promise((resolve, reject) =>
                         module.FS.syncfs(populate, error => error ? reject(error) : resolve()));
}
async function save() {
  if (active)
    await sync(active, false);
  status('SD saved in this browser');
}
function folder(module, path) { module.FS.mkdirTree(path); }
async function start(name, arg = '') {
  if (active || loading)
    throw new Error('Stop the current app first');
  loading = true;
  status(`Loading ${name}`);
  $('emulated').disabled = $('start').disabled = $('netsurf').disabled = true;
  await loadScript(name);
  const module =
      await modules[name]({canvas : $('canvas'), noInitialRun : true, print : log, printErr : log});
  module.paxTouch = {x : 0, y : 0, down : false};
  module.paxTouchQueue = [];
  const canvas = $('canvas');
  const pointer = (event, down) => {
    const box = canvas.getBoundingClientRect();
    module.paxTouch = {
      x : Math.max(0, Math.min(canvas.width - 1,
                               Math.floor((event.clientX - box.left) * canvas.width / box.width))),
      y : Math.max(0, Math.min(canvas.height - 1,
                               Math.floor((event.clientY - box.top) * canvas.height / box.height))),
      down
    };
  };
  canvas.onpointerdown = event => {
    canvas.setPointerCapture(event.pointerId);
    pointer(event, true);
    module.paxTouchQueue.push({...module.paxTouch});
    canvas.focus();
    event.preventDefault();
  };
  canvas.onpointermove = event => pointer(event, module.paxTouch.down);
  canvas.onpointerup = event => {
    pointer(event, false);
    module.paxTouchQueue.push({...module.paxTouch});
  };
  canvas.onpointercancel = () => { module.paxTouch.down = false; };
  attachPaxTransport(module);
  module.FS.mkdir('/sd');
  module.FS.mount(module.FS.filesystems.IDBFS, {}, '/sd');
  await sync(module, true);
  folder(module, '/sd/roms/pax');
  folder(module, '/sd/roms/netsurf');
  if (name === 'launcher' && modules.netsurf &&
      !module.FS.analyzePath('/sd/roms/pax/netsurf.pax').exists) {
    module.FS.writeFile('/sd/roms/pax/netsurf.pax', 'PAX Web module reference: netsurf\n');
    module.FS.writeFile('/sd/roms/pax/netsurf.json', JSON.stringify({
      name : 'netsurf',
      title : 'NetSurf',
      description : 'Browser source build',
      version : 'web'
    }));
  }
  const config = '/sd/roms/pax/.launcher_ui_network_url.txt';
  try {
    $('server').value = new TextDecoder().decode(module.FS.readFile(config)).trim();
  } catch {
  }
  module.paxOpen = (target, argument, back) => {
    const name = target.split('/').pop().replace(/\.pax$/, '');
    if (name !== 'netsurf' || !modules.netsurf) {
      log(`No WebAssembly build for ${target}. RISC-V PAX binaries require a separate emulator.`);
      return -1;
    }
    if (back) {
      if (returns.length >= 2)
        return -1;
      returns.push({name : currentApp, argument : module.paxResume || arg});
    }
    pending = {name, argument : argument === target ? "" : argument, back};
    return 0;
  };
  module.paxFinished = () => {
    setTimeout(async () => {
      try {
        await sync(module, false);
        for (const socket of Object.values(module.paxSockets))
          socket.abort.abort();
        const next = pending || returns.pop();
        pending = null;
        active = null;
        clearInterval(saveTimer);
        $('stop').disabled = true;
        $('start').disabled = false;
        $('emulated').disabled = false;
        $('netsurf').disabled = !modules.netsurf;
        status('App stopped; SD saved');
        if (next) {
          await start(next.name, next.argument);
        }
      } catch (error) {
        fail(error);
      }
    }, 0);
  };
  active = module;
  loading = false;
  currentApp = name;
  $('start').disabled = true;
  $('netsurf').disabled = true;
  $('stop').disabled = false;
  $('emulated').disabled = true;
  status(`${name} running`);
  saveTimer = setInterval(() => sync(module, false).catch(log), 15000);
  $('canvas').focus();
  module.callMain(arg ? [ arg ] : []);
}
function fail(error) {
  loading = false;
  log(error.stack || String(error));
  status('Error: ' + error.message);
  if (!active) {
    $('emulated').disabled = $('start').disabled = false;
    $('netsurf').disabled = !modules.netsurf;
  }
}
$('start').onclick = () => start('launcher').catch(fail);
$('netsurf').onclick = () => start('netsurf', $('page-url').value || 'about:welcome').catch(fail);
$('stop').onclick = () => {
  if (emulatedHost) {
    emulatedHost.quit = true;
    const cpu = emulatedCPU;
    setTimeout(() => {
      if (cpu === emulatedCPU)
        cpu.stopped = true;
    }, 2500);
  } else
    active?.ccall('pax_stop', null, [], []);
};
$('fullscreen').onclick = () => $('canvas').parentElement.requestFullscreen().catch(fail);
$('save').onclick = () => save().catch(fail);
$('apply').onclick = () => {
  try {
    if (!active)
      throw new Error('Start an app first');
    const url = new URL($('server').value);
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password)
      throw new Error('Use an HTTP or HTTPS URL without credentials');
    active.FS.writeFile('/sd/roms/pax/.launcher_ui_network_url.txt', url.href + '\n');
    save().catch(fail);
    log('Restart the launcher to use the saved development URL.');
  } catch (error) {
    fail(error);
  }
};
$('import').onchange = async event => {
  try {
    if (!active)
      throw new Error('Start an app first');
    for (const file of event.target.files) {
      const relative = file.webkitRelativePath.split('/').slice(1).join('/');
      if (!relative || relative.split('/').some(p => !p || p === '.' || p === '..') ||
          relative.includes('\\'))
        throw new Error('Invalid SD path');
      const path = '/sd/' + relative;
      try {
        active.FS.stat(path);
        log(`Preserved existing ${path}`);
        continue;
      } catch {
      }
      folder(active, path.slice(0, path.lastIndexOf('/')));
      active.FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
    }
    await save();
    log('SD folder imported. Refresh the app list to see imported files.');
  } catch (error) {
    fail(error);
  }
  event.target.value = '';
};
$('backup').onclick = () => {
  try {
    if (!active)
      throw new Error('Start an app first');
    const files = [];
    function walk(path) {
      for (const name of active.FS.readdir(path)) {
        if (name === '.' || name === '..')
          continue;
        const full = path + '/' + name, info = active.FS.stat(full);
        if (active.FS.isDir(info.mode))
          walk(full);
        else
          files.push({path : full.slice(4), bytes : Array.from(active.FS.readFile(full))});
      }
    }
    walk('/sd');
    const url = URL.createObjectURL(new Blob([ JSON.stringify({format : 'pax-web-sd-1', files}) ],
                                             {type : 'application/json'}));
    const link = document.createElement('a');
    link.href = url;
    link.download = 'pax-web-sd.json';
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  } catch (error) {
    fail(error);
  }
};
(async () => {
  try {
    const response = await fetch('netsurf.js', {method : 'HEAD'});
    if (response.ok) {
      await loadScript('netsurf');
      $('netsurf').disabled = false;
    }
  } catch {
  }
})();

async function startEmulated() {
  if (active || loading)
    throw new Error('Stop the current app first');
  loading = true;
  $('emulated').disabled = $('start').disabled = $('netsurf').disabled = true;
  status('Loading RISC-V runtime');
  await loadScript('launcher');
  if (!window.MUnicorn)
    await new Promise((resolve, reject) => {
      const s = document.createElement('script');
      s.src = 'unicorn_riscv.js';
      s.onload = resolve;
      s.onerror = () => reject(new Error('Unicorn runtime missing; run stage_emulator.py'));
      document.body.append(s);
    });
  const u = await window.MUnicorn(), fields = await (await fetch('pax-abi.json')).json(),
        boot = await (await fetch('emulator-build.json')).json();
  const module = await modules.launcher({noInitialRun : true, print : log, printErr : log});
  module.FS.mkdir('/sd');
  module.FS.mount(module.FS.filesystems.IDBFS, {}, '/sd');
  await sync(module, true);
  module.FS.mkdirTree('/sd/roms/pax');
  try {
    Object.assign(
        module.paxGuestSettings ??= {},
        JSON.parse(new TextDecoder().decode(module.FS.readFile('/sd/.pax-browser-settings.json'))));
  } catch {
  }
  const reference = '/sd/roms/pax/netsurf.pax';
  try {
    const old = module.FS.readFile(reference);
    if (old.length < 128 && new TextDecoder().decode(old).startsWith('PAX Web module reference:')) {
      module.FS.mkdirTree('/sd/.pax-web-references');
      module.FS.rename(reference, '/sd/.pax-web-references/netsurf.paxref');
      try {
        module.FS.rename(reference.replace('.pax', '.json'),
                         '/sd/.pax-web-references/netsurf.json');
      } catch {
      }
    }
  } catch {
  }
  let launcher;
  for (const app of boot.apps) {
    const bytes = await PaxBinary.verified(app.url, app.size, app.sha256);
    PaxBinary.validate(bytes);
    if (app.name === 'launcher_ui') {
      launcher = {bytes, target : '/sd/.pax-runtime/' + app.sha256 + '/launcher_ui.pax', arg : ''};
      module.FS.mkdirTree(launcher.target.slice(0, launcher.target.lastIndexOf('/')));
      module.FS.writeFile(launcher.target, bytes);
    } else {
      const path = '/sd/roms/pax/' + app.name + '.pax';
      if (!module.FS.analyzePath(path).exists) {
        module.FS.writeFile(path, bytes);
        const r = await fetch('pax-apps/' + app.name + '.json');
        if (r.ok)
          module.FS.writeFile(path.replace('.pax', '.json'), new Uint8Array(await r.arrayBuffer()));
      }
    }
  }
  active = module;
  loading = false;
  $('start').disabled = true;
  $('emulated').disabled = true;
  $('netsurf').disabled = true;
  $('stop').disabled = false;
  module.paxAudio = sharedAudio;
  saveTimer = setInterval(() => sync(module, false).catch(log), 15000);
  const stack = [];
  let job = launcher;
  try {
    while (job) {
      status('RISC-V: ' + job.target.split('/').pop());
      const host = emulatedHost = new PaxBrowserHost(module, $('canvas'), log);
      const cpu = emulatedCPU = new PaxEmulator(u, fields, host);
      $('canvas').focus();
      let result;
      try {
        result = await cpu.run(job.bytes, job.arg);
        log('RISC-V app returned ' + result);
        if (result < 0)
          log('Guest app reported a failure');
      } catch (error) {
        log('RISC-V execution failed: ' + error.message);
      }
      log(JSON.stringify(cpu.stats));
      await sync(module, false);
      if (host.handoff) {
        if (host.handoff.back) {
          if (stack.length >= 2)
            throw new Error('PAX return stack full');
          stack.push({...job, arg : host.resume});
        }
        job = host.handoff;
      } else
        job = stack.pop();
    }
  } finally {
    emulatedHost = emulatedCPU = null;
    active = null;
    clearInterval(saveTimer);
    $('start').disabled = false;
    $('emulated').disabled = false;
    $('netsurf').disabled = !modules.netsurf;
    $('stop').disabled = true;
    status('Emulator stopped; SD saved');
  }
}
$('emulated').onclick = () => startEmulated().catch(fail);
$('audio').onclick = async () => {
  try {
    sharedAudio ??= new AudioContext();
    await sharedAudio.resume();
    if (active)
      active.paxAudio = sharedAudio;
    if (emulatedHost) {
      emulatedHost.audio = sharedAudio;
      emulatedHost.audioAt = sharedAudio.currentTime;
      emulatedHost.audioSources ??= [];
    }
    status('Browser audio enabled');
  } catch (error) {
    fail(error);
  }
};
