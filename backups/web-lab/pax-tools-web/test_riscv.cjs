/* Exercise helper-heavy translated blocks without requiring a game ROM. */
const assert = require('node:assert/strict');
const {PaxEmulator} = require('./emulator.js');
(async () => {
  const u = await require(process.env.PAX_CPU_JS || '../../pax-dev/web/unicorn_riscv.js')();
  const fields = require('../../pax-dev/web/pax-abi.json');
  const words = [ (123 << 20) | (5 << 7) | 0x13, (7 << 20) | (6 << 7) | 0x13 ];
  // Each RV32 division requires the WASM adapter's paired helper arguments.
  for (let i = 0; i < 180; i++)
    words.push((1 << 25) | (6 << 20) | (5 << 15) | (5 << 12) | (7 << 7) | 0x33);
  words.push((0xfef << 20) | (7 << 15) | (10 << 7) | 0x13, 0x8067);
  const bytes = Buffer.alloc(32 + words.length * 4);
  [0x50415050, 1, 0, words.length * 4, 0, 0, 0, 0].forEach((v, i) => bytes.writeUInt32LE(v, i * 4));
  words.forEach((v, i) => bytes.writeUInt32LE(v >>> 0, 32 + i * 4));
  const host = {supports : () => false, bind() {}, cleanup() {}, log : console.log};
  const cpu = new PaxEmulator(u, fields, host);
  assert.equal(await cpu.run(bytes), 0);
  console.log('PASS: 180 RV32 divisions in one helper-heavy block, correct result and clean exit');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
