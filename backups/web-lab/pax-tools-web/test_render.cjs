/* Pixel-accurate rendering checks and repeatable browser service benchmarks. */
const assert = require('node:assert/strict');
const {performance} = require('node:perf_hooks');
global.attachPaxTransport = module => { module.paxSockets = {}; };
const {PaxBrowserHost} = require(process.env.PAX_HOST_JS || './emulator-host.js');
class Memory {
  constructor() {
    this.bytes = new Uint8Array(16 * 1024 * 1024);
    this.next = 16;
  }
  alloc(n) {
    const p = this.next;
    this.next += n;
    return p;
  }
  free() {}
  read(p, n) { return this.bytes.slice(p, p + n); }
  readInto(p, out) {
    out.set(this.bytes.subarray(p, p + out.length));
    return out;
  }
  write(p, b) { this.bytes.set(b, p); }
}
function create() {
  const mem = new Memory();
  let presentations = 0, lastDirty = null;
  const canvas = {
    style : {},
    addEventListener() {},
    removeEventListener() {},
    getContext : () => ({
      createImageData : (w, h) => ({data : new Uint8ClampedArray(w * h * 4)}),
      putImageData : (...args) => {
        presentations++;
        lastDirty = args.slice(3);
      }
    })
  };
  const host = new PaxBrowserHost({FS : {}}, canvas, () => {});
  host.bind({stats : {}}, mem, '');
  host.setCanvas(960, 600);
  return {
    host,
    mem,
    get presentations() { return presentations; },
    get lastDirty() { return lastDirty; }
  };
}
function reference(bytes, w, h, scale, swap, width, height) {
  const out = new Uint8Array(width * height * 2);
  const dw = Math.floor(w * scale), dh = Math.floor(h * scale), ox = Math.floor((width - dw) / 2),
        oy = Math.floor((height - dh) / 2);
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++) {
      if (x < ox || y < oy || x >= ox + dw || y >= oy + dh)
        continue;
      const from = (Math.floor((y - oy) / scale) * w + Math.floor((x - ox) / scale)) * 2,
            to = (y * width + x) * 2;
      out[to] = bytes[from + (swap ? 1 : 0)];
      out[to + 1] = bytes[from + (swap ? 0 : 1)];
    }
  return out;
}
function rgba(bytes) {
  const out = new Uint8ClampedArray(bytes.length * 2);
  for (let p = 0, i = 0; p < bytes.length; p += 2, i += 4) {
    const c = bytes[p] | bytes[p + 1] << 8;
    out[i] = (c >> 11) * 255 / 31;
    out[i + 1] = ((c >> 5) & 63) * 255 / 63;
    out[i + 2] = (c & 31) * 255 / 31;
    out[i + 3] = 255;
  }
  return out;
}
(async () => {
  const fixture = create(), {host, mem} = fixture, w = 320, h = 200,
        source = new Uint8Array(w * h * 2);
  for (let i = 0; i < source.length; i++)
    source[i] = (i * 31 + 17) & 255;
  const p = mem.alloc(source.length);
  mem.write(p, source);
  for (const [scale, swap] of [[ 1, false ], [ 2, false ], [ 3, false ], [ 1.25, true ],
                               [ 3.5, true ]]) {
    await host.call('display_write_frame_custom', [ p, w, h, scale, swap ]);
    const expected = reference(source, w, h, scale, swap, host.width, host.height);
    assert.deepEqual(mem.read(host.fbptr, expected.length), expected,
                     'Scaled guest framebuffer must match reference');
    assert.deepEqual(host.image.data, rgba(expected), 'RGB565 colors must remain byte-identical');
  }
  const pixel = mem.alloc(2);
  mem.write(pixel, new Uint8Array([ 0, 248 ]));
  await host.call('display_write_rect', [ 17, 23, 1, 1, pixel ]);
  await host.call('display_flush', []);
  assert.deepEqual(
      Array.from(host.image.data.slice((23 * host.width + 17) * 4, (23 * host.width + 17) * 4 + 4)),
      [ 255, 0, 0, 255 ]);
  if (!process.env.PAX_HOST_JS)
    assert.deepEqual(fixture.lastDirty, [ 17, 23, 1, 1 ]);
  const fb = await host.call('display_get_framebuffer', []);
  mem.write(fb, new Uint8Array([ 224, 7 ]));
  await host.call('display_flush', []);
  assert.deepEqual(Array.from(host.image.data.slice(0, 4)), [ 0, 255, 0, 255 ],
                   'Direct guest framebuffer writes must remain visible');
  const count = Number(process.env.PAX_RENDER_FRAMES || 100), started = performance.now();
  for (let i = 0; i < count; i++)
    await host.call('display_write_frame_custom', [ p, w, h, 3, false ]);
  const fullMs = performance.now() - started;
  host.directFramebuffer = false;
  const dirtyStarted = performance.now();
  for (let i = 0; i < count; i++) {
    await host.call('display_write_rect', [ 17, 23, 48, 44, p ]);
    await host.call('display_flush', []);
  }
  console.log(JSON.stringify({
    frames : count,
    fullFrameMs : fullMs,
    smallRectangleMs : performance.now() - dirtyStarted,
    presentations : fixture.presentations,
    pixels : 'identical'
  }));
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
