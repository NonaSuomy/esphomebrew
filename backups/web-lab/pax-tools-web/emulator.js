/* RV32 PAX execution using the pinned Unicorn WebAssembly CPU. */
const PaxBinary = {
  validate(bytes) {
    if (bytes.length < 32)
      throw new Error('PAX header is truncated');
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const h = Array.from({length : 8}, (_, i) => v.getUint32(i * 4, true));
    const [magic, abi, entry, text, data, bss, flags, res] = h;
    if (magic !== 0x50415050 || abi !== 1 || flags || res || bytes.length !== 32 + text + data ||
        entry >= text || text + data + bss > 128 * 1024 * 1024)
      throw new Error('Invalid or unsupported ABI-1 PAX binary');
    return {entry, text, data, bss};
  },
  async verified(url, size, hash, signal) {
    if (!Number.isSafeInteger(size) || size < 0 || size > 128 * 1024 * 1024 ||
        !/^[a-f0-9]{64}$/i.test(hash))
      throw new Error('Missing valid size/SHA-256');
    const r = await fetch(url, {credentials : 'omit', signal});
    if (!r.ok)
      throw new Error(`Download HTTP ${r.status}`);
    const chunks = [];
    let total = 0;
    const reader = r.body.getReader();
    for (;;) {
      const {done, value} = await reader.read();
      if (done)
        break;
      total += value.length;
      if (total > size) {
        await reader.cancel();
        throw new Error('Download exceeds declared size');
      }
      chunks.push(value);
    }
    if (total !== size)
      throw new Error('Download size mismatch');
    const bytes = new Uint8Array(total);
    let at = 0;
    for (const c of chunks) {
      bytes.set(c, at);
      at += c.length;
    }
    const digest = Array
                       .from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)),
                             v => v.toString(16).padStart(2, '0'))
                       .join('');
    if (digest !== hash.toLowerCase())
      throw new Error('Download SHA-256 mismatch');
    return bytes;
  }
};
class PaxMemory {
  constructor(u, e) {
    this.u = u;
    this.e = e;
    this.handle = u.getValue(e.handle_ptr, '*');
    this.scratch = u._malloc(8 * 1024 * 1024);
    this.next = 0x60000000;
    this.blocks = new Map();
    this.freed = [];
    e.mem_map(this.next, 128 * 1024 * 1024, u.PROT_ALL);
  }
  check(code) {
    if (code)
      throw new Error(this.u.strerror(code));
  }
  read(p, n) {
    if (!Number.isSafeInteger(n) || n < 0 || n > 128 * 1024 * 1024)
      throw new Error('Invalid memory read');
    return this.readInto(p, new Uint8Array(n));
  }
  readInto(p, out) {
    const n = out.length;
    for (let at = 0; at < n; at += 8 * 1024 * 1024) {
      const size = Math.min(n - at, 8 * 1024 * 1024);
      this.check(
          this.u._uc_mem_read(this.handle, BigInt((p + at) >>> 0), this.scratch, BigInt(size)));
      out.set(this.u.paxHeap().subarray(this.scratch, this.scratch + size), at);
    }
    return out;
  }
  write(p, bytes) {
    for (let at = 0; at < bytes.length; at += 8 * 1024 * 1024) {
      const b = bytes.subarray(at, at + 8 * 1024 * 1024);
      this.u.paxHeap().set(b, this.scratch);
      this.check(this.u._uc_mem_write(this.handle, BigInt((p + at) >>> 0), this.scratch,
                                      BigInt(b.length)));
    }
  }
  word(p) { return new DataView(this.read(p, 4).buffer).getUint32(0, true); }
  put(p, value) {
    if (p) {
      const b = new Uint8Array(4);
      new DataView(b.buffer).setUint32(0, value >>> 0, true);
      this.write(p, b);
    }
  }
  text(p, max = 16384) {
    if (!p)
      return '';
    const parts = [];
    for (let at = 0; at < max; at += 128) {
      const b = this.read(p + at, Math.min(128, max - at));
      const end = b.indexOf(0);
      parts.push(...(end < 0 ? b : b.subarray(0, end)));
      if (end >= 0)
        return new TextDecoder().decode(new Uint8Array(parts));
    }
    throw new Error('Unterminated guest string');
  }
  string(p, n, s) {
    if (p && n > 0) {
      const b = new TextEncoder().encode(s);
      const out = new Uint8Array(Math.min(n, b.length + 1));
      out.set(b.subarray(0, out.length - 1));
      this.write(p, out);
    }
  }
  alloc(n) {
    n = Math.ceil(Math.max(1, n) / 16) * 16;
    if (!Number.isSafeInteger(n) || n > 128 * 1024 * 1024)
      throw new Error('Invalid guest allocation');
    let p;
    const index = this.freed.findIndex(([, size ]) => size >= n);
    if (index >= 0) {
      const block = this.freed.splice(index, 1)[0];
      p = block[0];
      n = block[1];
    } else {
      p = this.next;
      this.next += n;
      if (this.next > 0x68000000)
        throw new Error('Guest heap exhausted');
    }
    this.blocks.set(p, n);
    this.write(p, new Uint8Array(n));
    return p;
  }
  free(p) {
    if (this.blocks.has(p)) {
      this.freed.push([ p, this.blocks.get(p) ]);
      this.blocks.delete(p);
    }
  }
  setReg(id, value) {
    new DataView(this.u.paxHeap().buffer).setUint32(this.scratch, value >>> 0, true);
    this.check(this.u._uc_reg_write(this.handle, id, this.scratch));
  }
  reg(id) {
    this.check(this.u._uc_reg_read(this.handle, id, this.scratch));
    return new DataView(this.u.paxHeap().buffer).getUint32(this.scratch, true);
  }
  float(id) {
    this.check(this.u._uc_reg_read(this.handle, id, this.scratch));
    return new DataView(this.u.paxHeap().buffer).getFloat32(this.scratch, true);
  }
  close() { this.u._free(this.scratch); }
}
class PaxEmulator {
  constructor(u, fields, host) {
    this.u = u;
    this.fields = fields;
    this.host = host;
    this.tasks = new Map();
    this.current = 0;
    this.stopped = false;
    this.stats = {services : {}, task_switches : 0, frames : 0, cpu_ms : 0, max_cpu_batch_ms : 0};
  }
  async run(bytes, arg = '') {
    const h = PaxBinary.validate(bytes), u = this.u;
    const e = this.e = new u.Unicorn(u.ARCH_RISCV, u.MODE_RISCV32);
    e.mem_map(0x4a000000, Math.ceil((h.text + h.data + h.bss + 4096) / 4096) * 4096, u.PROT_ALL);
    e.mem_write(0x4a000000, bytes.subarray(32));
    e.mem_map(0x70000000, 2 * 1024 * 1024, u.PROT_ALL);
    e.mem_map(0x10000, 0x10000, u.PROT_ALL);
    e.mem_write(0x10000, new Uint8Array(0x8000).map((_, i) => [0x67, 0x80, 0, 0][i % 4]));
    e.reg_write_i32(u.RISCV_REG_MSTATUS, 0x6000);
    const m = this.mem = new PaxMemory(u, e);
    this.host.bind(this, m, arg);
    const table = new Uint8Array(4 * (1 + this.fields.length + 64));
    const v = new DataView(table.buffer);
    v.setUint32(0, 1, true);
    this.traps = new Map();
    for (let i = 0; i < this.fields.length; i++) {
      const f = this.fields[i];
      if (this.host.supports(f.name) || [
            'ui_call_sync', 'task_create', 'task_delete', 'delay_ms', 'log_printf', 'log_vprintf'
          ].includes(f.name)) {
        const pc = 0x10100 + i * 4;
        v.setUint32((i + 1) * 4, pc, true);
        this.traps.set(pc, f);
      }
    }
    m.write(0x18000, table);
    e.reg_write_i32 = (id, value) => m.setReg(id, value);
    e.reg_write_i32(u.RISCV_REG_SP, 0x701ff000);
    e.reg_write_i32(u.RISCV_REG_RA, 0x10000);
    e.reg_write_i32(u.RISCV_REG_A0, 0x18000);
    const ctx = e.context_alloc();
    e.context_save(ctx);
    this.tasks.set(0, {ctx, pc : 0x4a000000 + h.entry, wake : 0, ui : []});
    let trap = null;
    const hook = e.hook_add(u.HOOK_CODE, (_, address) => {
      trap = Number(address);
      e.emu_stop();
    }, {}, 0x10000, 0x17fff);
    const channel = new MessageChannel();
    const yieldNow = () => new Promise(resolve => {
      channel.port1.onmessage = resolve;
      channel.port2.postMessage(0);
    });
    let failure = null;
    try {
      let quantum = performance.now();
      while (!this.stopped && this.tasks.size) {
        const t = this.tasks.get(this.current);
        if (!t) {
          this.schedule();
          continue;
        }
        if (t.wake > performance.now()) {
          this.schedule();
          const now = performance.now();
          if (this.tasks.get(this.current).wake > now) {
            const wake = Math.min(...Array.from(this.tasks.values(), task => task.wake));
            await new Promise(r => setTimeout(r, Math.max(1, Math.min(16, wake - now))));
          }
          continue;
        }
        trap = null;
        let scheduled = false;
        const started = performance.now();
        m.check(u._uc_emu_start(m.handle, BigInt(t.pc), 0xffffffffn, 0n, 100000));
        const cpuMs = performance.now() - started;
        this.stats.cpu_ms += cpuMs;
        this.stats.max_cpu_batch_ms = Math.max(this.stats.max_cpu_batch_ms, cpuMs);
        const ra = m.reg(u.RISCV_REG_RA);
        if (trap === 0x10000) {
          if (this.current === 0) {
            this.result = m.reg(u.RISCV_REG_A0) | 0;
            break;
          }
          this.deleteTask(this.current);
          this.schedule();
          scheduled = true;
        } else if (trap === 0x10008) {
          this.deleteTask(this.current);
          this.schedule();
          scheduled = true;
        } else if (trap === 0x10004) {
          if (!t.ui.length)
            throw new Error('Guest UI callback return stack empty');
          t.pc = t.ui.pop();
          e.reg_write_i32(u.RISCV_REG_RA, t.pc);
        } else if (trap !== null) {
          const f = this.traps.get(trap);
          if (!f)
            throw new Error(`Unknown service trap ${trap.toString(16)}`);
          this.stats.services[f.name] = (this.stats.services[f.name] || 0) + 1;
          let ri = 0, fi = 0;
          const a = f.args.map(type => {
            if (type === 'float')
              return m.float(u.RISCV_REG_F10 + fi++);
            let value = m.reg(u.RISCV_REG_A0 + ri++);
            return [ 'int', 'int32_t', 'long' ].includes(type) ? value | 0 : value;
          });
          if (f.name === 'ui_call_sync') {
            t.ui.push(ra);
            e.reg_write_i32(u.RISCV_REG_RA, 0x10004);
            e.reg_write_i32(u.RISCV_REG_A0, a[1]);
            t.pc = a[0];
          } else if (f.name === 'delay_ms') {
            t.pc = ra;
            t.wake = performance.now() + Math.max(1, a[0]);
            this.schedule();
            scheduled = true;
          } else if (f.name === 'task_delete') {
            t.pc = ra;
            this.deleteTask(a[0] || this.current);
            this.schedule();
            scheduled = true;
          } else {
            let result;
            if (f.name === 'task_create')
              result = this.createTask(a);
            else if (f.name === 'log_printf' || f.name === 'log_vprintf')
              result = this.formatLog(a[0], f.name === 'log_vprintf' ? a[1] : null);
            else {
              result = this.host.call(f.name, a);
              if (result && typeof result.then === 'function')
                result = await result;
            }
            if (f.ret !== 'void') {
              const value = BigInt(Math.trunc(result || 0));
              e.reg_write_i32(u.RISCV_REG_A0, Number(value & 0xffffffffn));
              if (f.ret === 'int64_t')
                e.reg_write_i32(u.RISCV_REG_A1, Number((value >> 32n) & 0xffffffffn));
            }
            t.pc = ra;
          }
        } else {
          t.pc = m.reg(u.RISCV_REG_PC);
          this.schedule();
          scheduled = true;
        }
        if (performance.now() - quantum > 7) {
          if (!scheduled)
            this.schedule();
          await yieldNow();
          quantum = performance.now();
        }
      }
      return this.result || 0;
    } catch (error) {
      failure = error;
      throw error;
    } finally {
      channel.port1.close();
      channel.port2.close();
      const cleanupErrors = [];
      const cleanup = fn => {
        try {
          fn();
        } catch (error) {
          cleanupErrors.push(error);
        }
      };
      cleanup(() => e.hook_del(hook));
      for (const t of this.tasks.values())
        cleanup(() => e.context_free(t.ctx));
      this.tasks.clear();
      cleanup(() => m.close());
      cleanup(() => e.close());
      cleanup(() => this.host.cleanup());
      if (!failure && cleanupErrors.length)
        throw new AggregateError(cleanupErrors, 'Guest cleanup failed');
    }
  }
  schedule() {
    if (!this.tasks.size)
      return;
    const old = this.tasks.get(this.current);
    const now = performance.now();
    let next = null, first = null, passedCurrent = false;
    for (const [id, task] of this.tasks) {
      if (task.wake > now)
        continue;
      if (first === null)
        first = id;
      if (passedCurrent) {
        next = id;
        break;
      }
      if (id === this.current)
        passedCurrent = true;
    }
    // A deleted current task has no place in the iteration order.
    if (next === null)
      next = first ?? (old ? this.current : this.tasks.keys().next().value);
    if (next === this.current && old)
      return;
    if (old)
      this.e.context_save(old.ctx);
    this.current = next;
    this.e.context_restore(this.tasks.get(next).ctx);
    this.stats.task_switches++;
  }
  createTask([ fn, name, depth, arg, priority, out, core ]) {
    if (!fn || depth > 1024 * 1024) {
      this.mem.put(out, 0);
      return -1;
    }
    const parent = this.tasks.get(this.current);
    this.e.context_save(parent.ctx);
    const size = Math.max(65536, depth * 4), stack = this.mem.alloc(size),
          handle = this.mem.alloc(4);
    this.mem.put(handle, handle);
    this.e.reg_write_i32(this.u.RISCV_REG_SP, (stack + size) & ~15);
    this.e.reg_write_i32(this.u.RISCV_REG_RA, 0x10008);
    this.e.reg_write_i32(this.u.RISCV_REG_A0, arg);
    const ctx = this.e.context_alloc();
    this.e.context_save(ctx);
    this.tasks.set(handle, {ctx, pc : fn, wake : 0, ui : [], stack});
    this.e.context_restore(parent.ctx);
    this.mem.put(out, handle);
    return 0;
  }
  deleteTask(handle) {
    const t = this.tasks.get(handle);
    if (t) {
      this.e.context_free(t.ctx);
      this.mem.free(t.stack || 0);
      this.mem.free(handle);
      this.tasks.delete(handle);
    }
  }
  formatLog(pointer, list) {
    const m = this.mem, fmt = m.text(pointer);
    let cursor = list === null ? 1 : 0;
    const word = () => list !== null ? m.word(list + cursor++ * 4)
                       : cursor < 8  ? m.reg(this.u.RISCV_REG_A0 + cursor++)
                                     : m.word(m.reg(this.u.RISCV_REG_SP) + (cursor++ - 8) * 4);
    const wide = () => {
      if (cursor % 2)
        cursor++;
      const low = word(), high = word();
      return (BigInt(high) << 32n) | BigInt(low);
    };
    const s = fmt.replace(
        /%([-+ #0]*)(\d+|\*)?(?:\.(\d+|\*))?(ll|l|z|h)?([%sdiuxXpcfg])/g,
        (_, flags, width, precision, length, kind) => {
          if (kind === '%')
            return '%';
          if (width === '*')
            width = word();
          if (precision === '*')
            precision = word();
          let value;
          if (kind === 'f' || kind === 'g') {
            const bits = wide(), b = new ArrayBuffer(8);
            new DataView(b).setBigUint64(0, bits, true);
            value = new DataView(b).getFloat64(0, true);
            return kind === 'f' ? value.toFixed(precision === undefined ? 6 : Number(precision))
                                : String(value);
          }
          value = length === 'll' ? wide() : word();
          let text = kind === 's'   ? m.text(Number(value))
                     : kind === 'c' ? String.fromCharCode(Number(value))
                     : kind === 'x' || kind === 'X' || kind === 'p' ? value.toString(16)
                     : kind === 'd' || kind === 'i'
                         ? String(length === 'll' ? BigInt.asIntN(64, value) : Number(value) | 0)
                         : String(value);
          if (kind === 'X')
            text = text.toUpperCase();
          if (kind === 's' && precision !== undefined)
            text = text.slice(0, Number(precision));
          return text.padStart(Math.min(64, Number(width) || 0), flags.includes('0') ? '0' : ' ');
        });
    this.host.log(s.trimEnd());
    return s.length;
  }
}
if (typeof module !== 'undefined')
  module.exports = {PaxBinary, PaxMemory, PaxEmulator};
