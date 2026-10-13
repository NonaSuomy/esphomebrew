/* Execute unchanged PAX binaries and validate browser installation contracts. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');
const zlib = require('node:zlib');
const {execFileSync} = require('node:child_process');
const ROOT = path.resolve(__dirname, '../..');
const {PaxEmulator, PaxBinary} = require('./emulator.js');
const {PaxBrowserHost} = require('./emulator-host.js');
global.PaxBinary = PaxBinary;
global.location = {
  href : 'http://localhost:8767/'
};
vm.runInThisContext(fs.readFileSync(path.join(__dirname, 'transport.js'), 'utf8'));
function rom() {
  const code = Buffer.from(
            '78d8a2ff9aa9008d00208d01202c022010fba93f8d0620a9008d0620a9218d0720a9088d0120', 'hex'),
        prg = Buffer.alloc(16384);
  code.copy(prg);
  const loop = 0x8000 + code.length;
  prg.set([ 0x4c, loop & 255, loop >> 8 ], code.length);
  for (let i = 0; i < 3; i++)
    prg.writeUInt16LE(0x8000, 16378 + i * 2);
  const header = Buffer.alloc(16);
  header.set([ 78, 69, 83, 26, 1, 1 ]);
  return Buffer.concat([ header, prg, Buffer.alloc(8192) ]);
}
function crc(bytes) {
  let c = 0xffffffff;
  for (const b of bytes) {
    c ^= b;
    for (let j = 0; j < 8; j++)
      c = (c >>> 1) ^ ((c & 1) ? 0xedb88320 : 0);
  }
  return (c ^ 0xffffffff) >>> 0;
}
function zip(bytes) {
  const name = Buffer.from('nested/test.NES'), data = zlib.deflateRawSync(bytes),
        local = Buffer.alloc(30), central = Buffer.alloc(46), end = Buffer.alloc(22),
        checksum = crc(bytes);
  local.writeUInt32LE(0x04034b50);
  local.writeUInt16LE(20, 4);
  local.writeUInt16LE(8, 8);
  local.writeUInt32LE(checksum, 14);
  local.writeUInt32LE(data.length, 18);
  local.writeUInt32LE(bytes.length, 22);
  local.writeUInt16LE(name.length, 26);
  central.writeUInt32LE(0x02014b50);
  central.writeUInt16LE(20, 4);
  central.writeUInt16LE(20, 6);
  central.writeUInt16LE(8, 10);
  central.writeUInt32LE(checksum, 16);
  central.writeUInt32LE(data.length, 20);
  central.writeUInt32LE(bytes.length, 24);
  central.writeUInt16LE(name.length, 28);
  end.writeUInt32LE(0x06054b50);
  end.writeUInt16LE(1, 8);
  end.writeUInt16LE(1, 10);
  end.writeUInt32LE(central.length + name.length, 12);
  end.writeUInt32LE(local.length + name.length + data.length, 16);
  return Buffer.concat([ local, name, data, central, name, end ]);
}
let coloredFrames = 0;
const canvas = {
  width : 800,
  height : 480,
  style : {},
  getContext : () => ({
    createImageData : (w, h) => ({data : new Uint8ClampedArray(w * h * 4)}),
    putImageData : image => {
      if (image.data.some((v, i) => i % 4 !== 3 && v !== 0))
        coloredFrames++;
    }
  }),
  addEventListener() {},
  removeEventListener() {}
};
(async () => {
  const target = process.argv[2] || 'touchtest';
  const module = await require(ROOT + '/pax-dev/web/launcher.js')(
      {noInitialRun : true, print : () => {}, printErr : console.error});
  module.FS.mkdir('/sd');
  module.FS.mkdirTree('/sd/roms/pax');
  const host = new PaxBrowserHost(module, canvas, console.log);
  if (target === 'install') {
    const bytes = fs.readFileSync(ROOT + '/pax-dev/apps/touchtest.pax'),
          hash = crypto.createHash('sha256').update(bytes).digest('hex');
    const info = {
      url : 'http://localhost/touchtest.pax',
      name : 'touchtest',
      title : 'Touch test',
      version : '0.2.0',
      size : bytes.length,
      sha256 : hash
    };
    global.fetch = async url => new Response(
        String(url).endsWith('.json') ? JSON.stringify({...info, file : 'touchtest.pax'}) : bytes);
    host.install = {state : 1, done : 0, total : bytes.length, status : ''};
    await host.installApp(info);
    assert.deepEqual(Buffer.from(module.FS.readFile('/sd/roms/pax/touchtest.pax')), bytes);
    assert.equal(host.install.state, 2);
    const before = module.FS.readFile('/sd/roms/pax/touchtest.pax');
    await assert.rejects(host.installApp({...info, sha256 : '0'.repeat(64)}), /SHA-256/);
    assert.deepEqual(module.FS.readFile('/sd/roms/pax/touchtest.pax'), before);
    const broken = Buffer.from(bytes);
    broken.writeUInt32LE(2, 4);
    assert.throws(() => PaxBinary.validate(broken), /ABI-1/);
    console.log('PASS: verified install, bad SHA/ABI rejection and preservation of installed copy');
    return;
  }
  let name = target;
  if (target.startsWith('nes')) {
    name = 'nes';
    module.FS.mkdirTree('/sd/roms/nes');
    const compressed = target === 'nes-zip', extension = compressed ? 'zip' : 'nes', data = rom();
    module.FS.writeFile('/sd/roms/nes/test.' + extension, compressed ? zip(data) : data);
    module.FS.writeFile('/sd/roms/pax/nes.rom', '/sd/roms/nes/test.' + extension + '\n');
  }
  if (target === 'doom') {
    module.FS.mkdirTree('/sd/roms/doom');
    const data = JSON.parse(fs.readFileSync(ROOT + '/pax-dev/apps/doom.json')).data.files;
    for (const file of data) {
      const bytes = fs.readFileSync(ROOT + '/pax-dev/desktop/sd/' + file.target);
      assert.equal(bytes.length, file.size);
      assert.equal(crypto.createHash('sha256').update(bytes).digest('hex'), file.sha256);
      module.FS.writeFile('/sd/' + file.target, bytes);
    }
  }
  const file = target.startsWith('launcher') ? ROOT + '/pax-dev/launcher_ui.pax'
                                             : ROOT + '/pax-dev/apps/' + name + '.pax';
  const call = host.call.bind(host);
  let audioFrames = 0, nonzeroAudio = 0;
  const isolation = target === 'launcher-isolation';
  let injected = false, injectedAt = 0, pendingAddress = 0;
  if (isolation) {
    const symbols = execFileSync(
        'readelf', [ '-sW', ROOT + '/pax-modules/.cache/native/launcher_ui/launcher_ui.elf' ],
        {encoding : 'utf8'});
    const match = symbols.match(/\d+:\s+([a-f0-9]+)\s+1\s+OBJECT[^\n]*g_archive_search_pending/);
    assert.ok(match, 'Build launcher ELF before testing its pending search state');
    pendingAddress = parseInt(match[1], 16);
  }
  host.call = async (name, args) => {
    if (name === 'audio_submit') {
      audioFrames += args[1];
      if (host.mem.read(args[0], args[1] * 4).some(v => v !== 0))
        nonzeroAudio++;
    }
    const result = await call(name, args);
    if (isolation && host.frames && !injected) {
      host.mem.write(pendingAddress, new Uint8Array([ 1 ]));
      injected = true;
      injectedAt = performance.now();
    }
    if (isolation ? injected && performance.now() - injectedAt > 2000
                  : host.frames >= (target === 'doom' ? 8 : 3))
      host.quit = true;
    return result;
  };
  const u = await require(ROOT + '/pax-dev/web/unicorn_riscv.js')(),
        fields = JSON.parse(fs.readFileSync(ROOT + '/pax-dev/web/pax-abi.json')),
        cpu = new PaxEmulator(u, fields, host);
  const timeout = setTimeout(() => { cpu.stopped = true; }, target === 'doom' ? 120000 : 30000);
  const result = await cpu.run(fs.readFileSync(file));
  clearTimeout(timeout);
  assert.equal(result, 0);
  assert.ok(cpu.stats.frames >= (isolation ? 2 : 3), 'App must render actual frames');
  assert.ok(coloredFrames > 0, 'App must render nonblack content');
  assert.equal(cpu.stopped, false, 'App must exit cooperatively');
  if (isolation) {
    assert.equal(injected, true);
    assert.equal(cpu.stats.services.net_tls_connect || 0, 0,
                 'Pending Archive search must not run on SD');
  }
  if (target === 'nes-zip')
    assert.equal(cpu.stats.services.file_zip_extract_first, 1);
  if (name === 'nes' || name === 'doom')
    assert.ok(cpu.stats.services.task_create > 0);
  if (name === 'doom')
    assert.ok(audioFrames > 0);
  console.log(
      JSON.stringify({target, result, ...cpu.stats, audioFrames, nonzeroAudio, coloredFrames}));
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
