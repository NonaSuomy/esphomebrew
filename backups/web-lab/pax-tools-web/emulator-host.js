/* Browser services for unchanged ABI-1 PAX binaries. */
class PaxBrowserHost {
  constructor(module, canvas, log = console.log) {
    this.module = module;
    this.FS = module.FS;
    this.canvas = canvas;
    this.log = log;
    this.handles = new Map();
    this.nextHandle = 1;
    this.hostnames = new Map();
    this.nextIP = 1;
    this.keys = [];
    this.pad = Array(14).fill(0);
    this.pointer = {x : 0, y : 0, down : false};
    this.touches = [];
    this.wheel = 0;
    this.install = {state : 0, done : 0, total : 0, result : 0, status : ''};
    this.quit = false;
    this.frames = 0;
    this.canvasChosen = false;
    this.width = 800;
    this.height = 480;
    this.settings = module.paxGuestSettings ??=
        {volume : 75, brightness : 100, auto_brightness : 0, rom_path : ''};
    attachPaxTransport(module);
  }
  supports(name) {
    return new Set([
             'display_get_framebuffer',
             'display_get_emu_buffer',
             'display_flush',
             'display_emu_flush',
             'display_clear',
             'display_set_scale',
             'display_write_frame_rgb565',
             'display_write_frame_custom',
             'display_write_rect',
             'display_lock',
             'display_unlock',
             'audio_init',
             'audio_submit',
             'input_gamepad_read',
             'input_l3_read',
             'touch_read',
             'input_keyboard_read',
             'input_mouse_read',
             'input_mouse_scroll_read',
             'file_open',
             'file_close',
             'file_read',
             'file_write',
             'file_seek',
             'file_tell',
             'file_list_dir',
             'file_stat',
             'file_mkdir',
             'file_remove',
             'file_rename',
             'dir_open',
             'dir_read',
             'dir_close',
             'file_zip_extract_first',
             'file_zip_extract_named',
             'mem_alloc',
             'mem_calloc',
             'mem_realloc',
             'mem_free',
             'mem_caps_alloc',
             'get_time_us',
             'settings_rom_path_get',
             'settings_rom_path_set',
             'settings_volume_get',
             'settings_volume_set',
             'settings_brightness_get',
             'settings_brightness_set',
             'settings_auto_brightness_get',
             'settings_auto_brightness_set',
             'net_resolve',
             'net_tcp_connect',
             'net_tcp_send',
             'net_tcp_recv',
             'net_poll',
             'net_udp_close',
             'net_tls_connect',
             'net_tls_status',
             'net_tls_send',
             'net_tls_recv',
             'net_tls_close',
             'net_ipv4',
             'display_get_size',
             'display_set_canvas',
             'app_open',
             'app_get_arg',
             'app_set_resume_arg',
             'archive_auth_get',
             'network_config_get',
             'device_status_get',
             'storage_status_get',
             'ui_sound',
             'store_install_start',
             'store_install_status'
           ])
        .has(name);
  }
  bind(cpu, memory, arg) {
    this.cpu = cpu;
    this.mem = memory;
    this.argument = arg;
    this.resume = arg;
    this.start = performance.now();
    this.setCanvas(800, 480);
    this.bindInput();
  }
  path(path) {
    if (typeof path !== 'string' || !(path === '/sd' || path.startsWith('/sd/')) ||
        path.includes('\\') || path.includes(':') ||
        path.split('/').some(v => v === '.' || v === '..'))
      throw new Error('Invalid browser SD path');
    return path.replace(/\/+$/, '');
  }
  text(p) { return this.mem.text(p); }
  write(p, b) {
    if (p)
      this.mem.write(p, b);
  }
  setCanvas(w, h) {
    this.mem.free(this.fbptr || 0);
    this.width = w;
    this.height = h;
    this.fbptr = this.mem.alloc(w * h * 2);
    this.canvas.width = w;
    this.canvas.height = h;
    this.canvas.style.aspectRatio = `${w}/${h}`;
    this.context = this.canvas.getContext('2d');
    this.image = this.context.createImageData(w, h);
  }
  render() {
    const data = this.mem.read(this.fbptr, this.width * this.height * 2), out = this.image.data;
    for (let p = 0, i = 0; p < data.length; p += 2, i += 4) {
      const c = data[p] | data[p + 1] << 8;
      out[i] = (c >> 11) * 255 / 31;
      out[i + 1] = ((c >> 5) & 63) * 255 / 63;
      out[i + 2] = (c & 31) * 255 / 31;
      out[i + 3] = 255;
    }
    this.context.putImageData(this.image, 0, 0);
    this.frames++;
    this.cpu.stats.frames = this.frames;
  }
  bindInput() {
    const c = this.canvas;
    const point = e => {
      const r = c.getBoundingClientRect();
      return {
        x : Math.max(
            0, Math.min(this.width - 1, Math.floor((e.clientX - r.left) * this.width / r.width))),
        y : Math.max(
            0, Math.min(this.height - 1, Math.floor((e.clientY - r.top) * this.height / r.height)))
      };
    };
    c.onpointerdown = e => {
      c.setPointerCapture(e.pointerId);
      this.pointer = {...point(e), down : true};
      this.touches.push({...this.pointer});
      c.focus();
      e.preventDefault();
    };
    c.onpointermove = e => { this.pointer = {...point(e), down : this.pointer.down}; };
    c.onpointerup = e => {
      this.pointer = {...point(e), down : false};
      this.touches.push({...this.pointer});
    };
    c.onpointercancel = () => {
      this.pointer.down = false;
      this.touches.push({...this.pointer});
    };
    this.keyDown = e => this.key(e, true);
    this.keyUp = e => this.key(e, false);
    c.addEventListener('keydown', this.keyDown);
    c.addEventListener('keyup', this.keyUp);
    this.scroll = e => {
      this.wheel += e.deltaY < 0 ? 1 : -1;
      e.preventDefault();
    };
    c.addEventListener('wheel', this.scroll, {passive : false});
    this.blur = () => {
      this.pad.fill(0);
      this.pointer.down = false;
    };
    c.addEventListener('blur', this.blur);
  }
  key(e, down) {
    const special = {
      ArrowUp : 153,
      ArrowRight : 154,
      ArrowDown : 155,
      ArrowLeft : 156,
      Enter : 13,
      Backspace : 127,
      Control : 133,
      Alt : 132,
      Shift : 134,
      Escape : 27
    };
    const key = special[e.key] ?? (e.key.length === 1 ? e.key.charCodeAt(0) : 0);
    if (key) {
      this.keys.push({key, down});
      if (this.keys.length > 256)
        this.keys.shift();
    }
    const map =
        {w : 0, d : 1, s : 2, a : 3, Tab : 4, F1 : 5, j : 6, k : 7, u : 8, i : 9, q : 10, e : 11};
    const index = map[e.key];
    if (index !== undefined)
      this.pad[index] = down ? 1 : 0;
    if (e.key === 'Escape' && down)
      this.quit = true;
    if (key || index !== undefined)
      e.preventDefault();
  }
  cleanup() {
    this.closed = true;
    this.installAbort?.abort();
    for (const h of this.handles.values())
      if (h.kind === 'file')
        try {
          this.FS.close(h.stream);
        } catch {
        }
    for (const s of Object.values(this.module.paxSockets))
      s.abort.abort();
    this.canvas.removeEventListener('keydown', this.keyDown);
    this.canvas.removeEventListener('keyup', this.keyUp);
    this.canvas.removeEventListener('wheel', this.scroll);
    this.canvas.removeEventListener('blur', this.blur);
    if (this.audio)
      for (const s of this.audioSources || [])
        try {
          s.stop();
        } catch {
        }
  }
  async loadTarget(target) {
    if (/^https?:\/\//.test(target)) {
      const url = new URL(target, location.href);
      let metadata = null;
      try {
        const side = new URL(url);
        side.pathname = side.pathname.replace(/\.pax$/, '.json');
        const r = await fetch(side, {credentials : 'omit'});
        if (r.ok)
          metadata = await r.json();
      } catch {
      }
      let bytes;
      if (metadata?.size && metadata?.sha256)
        bytes = await PaxBinary.verified(url.href, metadata.size, metadata.sha256);
      else {
        const r = await fetch(url, {credentials : 'omit'});
        if (!r.ok)
          throw new Error(`PAX HTTP ${r.status}`);
        const limit = 128 * 1024 * 1024, chunks = [];
        let size = 0;
        const reader = r.body.getReader();
        for (;;) {
          const {done, value} = await reader.read();
          if (done)
            break;
          size += value.length;
          if (size > limit) {
            await reader.cancel();
            throw new Error('PAX download too large');
          }
          chunks.push(value);
        }
        bytes = new Uint8Array(size);
        let p = 0;
        for (const b of chunks) {
          bytes.set(b, p);
          p += b.length;
        }
      }
      PaxBinary.validate(bytes);
      return bytes;
    }
    let path = target;
    if (!path.startsWith('/'))
      path = '/sd/roms/pax/' + path.replace(/\.pax$/, '') + '.pax';
    const bytes = this.FS.readFile(this.path(path));
    PaxBinary.validate(bytes);
    return bytes;
  }
  async call(name, a) {
    const m = this.mem, F = this.FS, T = p => this.text(p);
    if (name.startsWith('mem_')) {
      if (name === 'mem_free') {
        m.free(a[0]);
        return 0;
      }
      if (name === 'mem_calloc')
        return m.alloc(a[0] * a[1]);
      if (name === 'mem_realloc') {
        const [p, n] = a, q = m.alloc(n);
        if (p) {
          m.write(q, m.read(p, Math.min(n, m.blocks.get(p) || 0)));
          m.free(p);
        }
        return q;
      }
      return m.alloc(a[0]);
    }
    if (name === 'get_time_us')
      return Math.floor((performance.now() - this.start) * 1000);
    if (name === 'display_get_size') {
      m.put(a[0], this.canvasChosen ? this.width : 1024);
      m.put(a[1], this.canvasChosen ? this.height : 600);
      return 0;
    }
    if (name === 'display_set_canvas') {
      const [w, h] = a;
      if (w < 320 || h < 240 || w > 1024 || h > 600 || w % 2 || h % 2)
        return -1;
      if (w !== this.width || h !== this.height)
        this.setCanvas(w, h);
      this.canvasChosen = true;
      return 0;
    }
    if (name === 'display_get_framebuffer' || name === 'display_get_emu_buffer')
      return this.fbptr;
    if (name === 'display_clear') {
      const out = new Uint8Array(this.width * this.height * 2);
      for (let p = 0; p < out.length; p += 2) {
        out[p] = a[0] & 255;
        out[p + 1] = a[0] >> 8;
      }
      m.write(this.fbptr, out);
      return 0;
    }
    if (name === 'display_write_rect') {
      const [x, y, w, h, p] = a;
      if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > this.width || y + h > this.height)
        throw new Error('Display rectangle outside canvas');
      const data = m.read(p, w * h * 2);
      for (let r = 0; r < h; r++)
        m.write(this.fbptr + ((y + r) * this.width + x) * 2,
                data.subarray(r * w * 2, (r + 1) * w * 2));
      return 0;
    }
    if (name === 'display_write_frame_rgb565') {
      m.write(this.fbptr, m.read(a[0], this.width * this.height * 2));
      return 0;
    }
    if (name === 'display_write_frame_custom') {
      const [p, w, h, scale, swap] = a;
      if (!w || !h || w > 2048 || h > 2048 || !(scale > 0 && scale <= 16))
        throw new Error('Invalid scaled framebuffer');
      const source = m.read(p, w * h * 2), out = new Uint8Array(this.width * this.height * 2),
            dw = Math.floor(w * scale), dh = Math.floor(h * scale),
            ox = Math.floor((this.width - dw) / 2), oy = Math.floor((this.height - dh) / 2);
      for (let y = 0; y < this.height; y++)
        for (let x = 0; x < this.width; x++) {
          if (x < ox || y < oy || x >= ox + dw || y >= oy + dh)
            continue;
          const from = (Math.floor((y - oy) / scale) * w + Math.floor((x - ox) / scale)) * 2,
                to = (y * this.width + x) * 2;
          out[to] = source[from + (swap ? 1 : 0)];
          out[to + 1] = source[from + (swap ? 0 : 1)];
        }
      m.write(this.fbptr, out);
      this.render();
      return 0;
    }
    if (name === 'display_flush' || name === 'display_emu_flush') {
      this.render();
      return 0;
    }
    if ([ 'display_lock', 'display_unlock', 'display_set_scale', 'ui_sound' ].includes(name))
      return 0;
    if (name === 'input_gamepad_read') {
      const b = new Uint8Array(14 * 4), v = new DataView(b.buffer);
      this.pad.forEach((value, i) => v.setInt32(i * 4, value, true));
      if (this.quit)
        v.setInt32(8 * 4, 1, true);
      m.write(a[0], b);
      return 0;
    }
    if (name === 'input_l3_read')
      return this.quit ? 1 : 0;
    if (name === 'touch_read') {
      const p = this.touches.shift() || this.pointer;
      m.put(a[0], p.x);
      m.put(a[1], p.y);
      return p.down ? 1 : 0;
    }
    if (name === 'input_keyboard_read') {
      const key = this.keys.shift();
      if (!key)
        return 0;
      m.put(a[0], key.key);
      m.put(a[0] + 4, key.down ? 1 : 0);
      return 1;
    }
    if (name === 'input_mouse_read') {
      a.forEach(p => m.put(p, 0));
      return 0;
    }
    if (name === 'input_mouse_scroll_read') {
      m.put(a[0], this.wheel);
      const moved = this.wheel !== 0;
      this.wheel = 0;
      return moved ? 1 : 0;
    }
    if (name === 'audio_init') {
      this.sampleRate = a[0];
      this.audio = this.module.paxAudio || null;
      this.audioAt = this.audio?.currentTime || 0;
      this.audioSources = [];
      return 0;
    }
    if (name === 'audio_submit') {
      if (this.audio?.state !== 'running')
        return 0;
      const frames = a[1];
      if (frames < 1 || frames > 65536)
        return 0;
      const bytes = m.read(a[0], frames * 4), v = new DataView(bytes.buffer),
            buffer = this.audio.createBuffer(2, frames, this.sampleRate);
      for (let c = 0; c < 2; c++) {
        const out = buffer.getChannelData(c);
        for (let i = 0; i < frames; i++)
          out[i] = v.getInt16(i * 4 + c * 2, true) / 32768;
      }
      if (this.audioAt < this.audio.currentTime)
        this.audioAt = this.audio.currentTime + 0.03;
      if (this.audioAt > this.audio.currentTime + 0.5)
        return 0;
      const src = this.audio.createBufferSource();
      src.buffer = buffer;
      src.connect(this.audio.destination);
      src.start(this.audioAt);
      this.audioAt += frames / this.sampleRate;
      this.audioSources.push(src);
      src.onended = () => {
        const i = this.audioSources.indexOf(src);
        if (i >= 0)
          this.audioSources.splice(i, 1);
      };
      return 0;
    }
    if (name.startsWith('settings_')) {
      const key = name.slice(9, -4);
      if (name.endsWith('_get')) {
        if (key === 'rom_path') {
          const bytes = new TextEncoder().encode(this.settings[key] || '');
          const p = m.alloc(bytes.length + 1);
          m.string(p, bytes.length + 1, this.settings[key] || '');
          return p;
        }
        return this.settings[key] || 0;
      }
      this.settings[key] = key === 'rom_path' ? T(a[0]) : a[0];
      F.writeFile('/sd/.pax-browser-settings.json', JSON.stringify(this.settings));
      return 0;
    }
    if (name.startsWith('file_') || name.startsWith('dir_')) {
      try {
        if (name === 'file_open') {
          const path = this.path(T(a[0])), mode = T(a[1]).replace(/[bt]/g, ''),
                stream = F.open(path, mode);
          const id = this.nextHandle++;
          this.handles.set(id, {kind : 'file', stream});
          return id;
        }
        if (name === 'file_close') {
          F.close(this.handles.get(a[0]).stream);
          this.handles.delete(a[0]);
          return 0;
        }
        if (name === 'file_read') {
          const size = a[1], count = a[2], bytes = new Uint8Array(size * count);
          const n = F.read(this.handles.get(a[3]).stream, bytes, 0, bytes.length);
          m.write(a[0], bytes.subarray(0, n));
          return size ? Math.floor(n / size) : 0;
        }
        if (name === 'file_write') {
          const b = m.read(a[0], a[1] * a[2]);
          const n = F.write(this.handles.get(a[3]).stream, b, 0, b.length);
          return a[1] ? Math.floor(n / a[1]) : 0;
        }
        if (name === 'file_seek') {
          F.llseek(this.handles.get(a[0]).stream, a[1], a[2]);
          return 0;
        }
        if (name === 'file_tell')
          return this.handles.get(a[0]).stream.position;
        if (name === 'file_mkdir') {
          F.mkdirTree(this.path(T(a[0])));
          return 0;
        }
        if (name === 'file_remove') {
          const p = this.path(T(a[0]));
          if (p === '/sd')
            return -1;
          const st = F.stat(p);
          F.isDir(st.mode) ? F.rmdir(p) : F.unlink(p);
          return 0;
        }
        if (name === 'file_rename') {
          const p = this.path(T(a[0])), q = this.path(T(a[1]));
          if (p === '/sd' || q === '/sd')
            return -1;
          try {
            F.stat(q);
            return -1;
          } catch {
          }
          F.rename(p, q);
          return 0;
        }
        if (name === 'file_stat') {
          const b = new Uint8Array(24);
          m.write(a[1], b);
          const st = F.stat(this.path(T(a[0]))), v = new DataView(b.buffer);
          v.setBigUint64(0, BigInt(st.size), true);
          v.setBigInt64(8, BigInt(Math.floor(st.mtime.getTime() / 1000)), true);
          v.setUint32(16, F.isDir(st.mode) ? 1 : 0, true);
          m.write(a[1], b);
          return 0;
        }
        if (name === 'file_list_dir') {
          const p = this.path(T(a[0]));
          let written = 0, count = 0;
          for (const n of F.readdir(p)) {
            if (n === '.' || n === '..')
              continue;
            const dir = F.isDir(F.stat(p + '/' + n).mode),
                  b = new TextEncoder().encode(n + (dir ? '/' : '') + '\0');
            if (written + b.length > a[2])
              continue;
            m.write(a[1] + written, b);
            written += b.length;
            count++;
          }
          return count;
        }
        if (name === 'dir_open') {
          const p = this.path(T(a[0])), id = this.nextHandle++;
          this.handles.set(
              id,
              {kind : 'dir', path : p, names : F.readdir(p).filter(n => n !== '.' && n !== '..')});
          return id;
        }
        if (name === 'dir_read') {
          const d = this.handles.get(a[0]), n = d.names.shift();
          if (n === undefined)
            return 0;
          m.string(a[1], a[2], n);
          m.put(a[3], F.isDir(F.stat(d.path + '/' + n).mode) ? 1 : 0);
          return 1;
        }
        if (name === 'dir_close') {
          this.handles.delete(a[0]);
          return 0;
        }
        if (name.startsWith('file_zip_'))
          return await this.zip(name, a);
      } catch (error) {
        if (name === 'file_open')
          return 0;
        this.log(`${name}: ${error.message}`);
        return [ 'file_read', 'file_write', 'dir_open' ].includes(name) ? 0 : -1;
      }
    }
    if (name === 'net_resolve') {
      const host = T(a[0]);
      let ip = [...this.hostnames ].find(([, h ]) => h === host)?.[0];
      if (!ip) {
        ip = (0xf0000000 | this.nextIP++) >>> 0;
        this.hostnames.set(ip, host);
      }
      m.put(a[1], ip);
      return 1;
    }
    if (name === 'net_tcp_connect') {
      const ip = a[0],
            host = this.hostnames.get(ip) ||
                   [ ip >>> 24, (ip >>> 16) & 255, (ip >>> 8) & 255, ip & 255 ].join('.');
      return this.module.paxConnect(host, a[1], false);
    }
    if (name === 'net_tls_connect')
      return this.module.paxConnect(T(a[0]), a[1], true);
    if (name === 'net_tcp_send' || name === 'net_tls_send')
      return this.module.paxSend(a[0], m.read(a[1], a[2]));
    if (name === 'net_tcp_recv' || name === 'net_tls_recv') {
      const bytes = new Uint8Array(a[2]);
      const n = this.module.paxRecv(a[0], 0, a[2], name === 'net_tls_recv', bytes);
      if (n > 0)
        m.write(a[1], bytes.subarray(0, n));
      return n;
    }
    if (name === 'net_tls_status') {
      const s = this.module.paxSockets[a[0]];
      return !s || s.error ? -1 : 1;
    }
    if (name === 'net_poll') {
      const s = this.module.paxSockets[a[0]];
      return !s ? -1 : s.error ? 4 : 2 | (s.response ? 1 : 0);
    }
    if (name === 'net_udp_close' || name === 'net_tls_close') {
      const s = this.module.paxSockets[a[0]];
      if (s)
        s.abort.abort();
      delete this.module.paxSockets[a[0]];
      return 0;
    }
    if (name === 'net_ipv4') {
      m.put(a[0], 0);
      m.put(a[1], 0);
      return 1;
    }
    if (name === 'app_get_arg') {
      m.string(a[0], a[1], this.argument);
      return new TextEncoder().encode(this.argument).length;
    }
    if (name === 'app_set_resume_arg') {
      const text = T(a[0]);
      if (new TextEncoder().encode(text).length >= 2048)
        return -1;
      this.resume = text;
      return 0;
    }
    if (name === 'app_open') {
      if (this.handoff)
        return -1;
      try {
        const target = T(a[0]), bytes = await this.loadTarget(target);
        let arg = T(a[1]);
        if (target.endsWith('/netsurf.pax') && arg === target)
          arg = '';
        this.handoff = {bytes, target, arg, back : !!a[2]};
        this.quit = true;
        return 0;
      } catch (error) {
        this.log(`app_open: ${error.message}`);
        return -1;
      }
    }
    if (name === 'archive_auth_get') {
      m.string(a[0], a[1], '');
      return 0;
    }
    if (name === 'network_config_get') {
      const b = new Uint8Array(28);
      new DataView(b.buffer).setInt32(
          4, typeof navigator === 'undefined' || navigator.onLine ? 1 : 0, true);
      m.write(a[0], b);
      return 0;
    }
    if (name === 'device_status_get') {
      const b = new Uint8Array(24);
      const time =
          new Date().toLocaleTimeString([], {hour : '2-digit', minute : '2-digit', hour12 : false});
      b.set(new TextEncoder().encode(time).subarray(0, 15));
      new DataView(b.buffer).setInt32(20, -1, true);
      m.write(a[0], b);
      return 0;
    }
    if (name === 'storage_status_get') {
      const b = new Uint8Array(40), v = new DataView(b.buffer);
      if (a[0] === 0) {
        v.setInt32(0, 1, true);
        v.setInt32(4, 1, true);
        b.set(new TextEncoder().encode('/sd'), 24);
      }
      m.write(a[1], b);
      return 0;
    }
    if (name === 'store_install_start') {
      if (this.install.state === 1)
        return -2;
      this.install =
          {state : 1, done : 0, total : a[4], result : 0, status : 'Downloading and verifying PAX'};
      this.installAbort = new AbortController();
      this.installApp({
            url : T(a[0]),
            name : T(a[1]),
            title : T(a[2]),
            version : T(a[3]),
            size : a[4],
            sha256 : T(a[5])
          })
          .catch(error => {
            this.install.state = 3;
            this.install.result = -1;
            this.install.status = error.message;
            this.log('Install failed: ' + error.message);
          });
      return 0;
    }
    if (name === 'store_install_status') {
      const b = new Uint8Array(112), v = new DataView(b.buffer), p = this.install;
      v.setUint32(0, p.state, true);
      v.setUint32(4, p.done, true);
      v.setUint32(8, p.total, true);
      v.setInt32(12, p.result, true);
      b.set(new TextEncoder().encode(p.status).subarray(0, 95), 16);
      m.write(a[0], b);
      return 0;
    }
    throw new Error('Unimplemented guest service: ' + name);
  }
  async installApp(info) {
    if (!/^[a-zA-Z0-9_-]{1,64}$/.test(info.name))
      throw new Error('Invalid app name');
    const url = new URL(info.url, location.href);
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password)
      throw new Error('Invalid install URL');
    const bytes =
        await PaxBinary.verified(url.href, info.size, info.sha256, this.installAbort?.signal);
    PaxBinary.validate(bytes);
    let metadata = {...info, file : info.name + '.pax'};
    const side = new URL(url);
    side.pathname = side.pathname.replace(/\.pax$/, '.json');
    const r = await fetch(side, {credentials : 'omit', signal : this.installAbort?.signal});
    if (r.ok) {
      const json = await r.json();
      if (json.name !== info.name || json.sha256 !== info.sha256 || json.size !== info.size)
        throw new Error('App JSON does not match verified binary');
      metadata = json;
    } else if (r.status !== 404)
      throw new Error('App JSON download failed');
    const staged = [];
    for (const file of metadata.data?.files || []) {
      const path = this.path('/sd/' + file.target);
      let dataUrl = file.url;
      if (!dataUrl) {
        const source = metadata.data;
        if (!/^https:\/\/github.com\/[\w.-]+\/[\w.-]+\/?$/.test(source.repo) ||
            !/^[a-f0-9]{40}$/i.test(source.ref))
          throw new Error('Invalid pinned data source');
        dataUrl =
            source.repo.replace('github.com', 'raw.githubusercontent.com').replace(/\/$/, '') +
            '/' + source.ref + '/' + file.path;
      }
      this.install.status = 'Verifying ' + file.target;
      const data =
          await PaxBinary.verified(dataUrl, file.size, file.sha256, this.installAbort?.signal);
      staged.push({path, bytes : data});
      this.install.total += file.size;
      this.install.done += file.size;
    }
    if (this.closed || this.installAbort?.signal.aborted)
      throw new Error('Install cancelled');
    const path = '/sd/roms/pax/' + info.name + '.pax';
    staged.push({path, bytes});
    staged.push({
      path : path.replace(/\.pax$/, '.json'),
      bytes : new TextEncoder().encode(JSON.stringify(metadata))
    });
    for (const file of staged) {
      this.FS.mkdirTree(file.path.slice(0, file.path.lastIndexOf('/')));
      this.FS.writeFile(file.path + '.part', file.bytes);
    }
    for (const file of staged) {
      if (this.FS.analyzePath(file.path).exists) {
        const old = this.FS.readFile(file.path);
        const hash = Array
                         .from(new Uint8Array(await crypto.subtle.digest('SHA-256', old)),
                               v => v.toString(16).padStart(2, '0'))
                         .join('');
        const backup = '/sd/.pax-backups/' + hash + '/' + file.path.split('/').pop();
        this.FS.mkdirTree(backup.slice(0, backup.lastIndexOf('/')));
        this.FS.writeFile(backup, old);
      }
    }
    for (const file of staged)
      this.FS.rename(file.path + '.part', file.path);
    await new Promise((resolve, reject) =>
                          this.FS.syncfs(false, error => error ? reject(error) : resolve()));
    this.install.done = this.install.total;
    this.install.state = 2;
    this.install.result = 0;
    this.install.status = 'Installed and verified';
    this.log(`Installed ${info.name} ${info.version}: SHA-256 verified`);
  }
  async zip(name, a) {
    const bytes = this.FS.readFile(this.path(this.text(a[0]))),
          v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let end = bytes.length - 22;
    while (end >= Math.max(0, bytes.length - 65557) && v.getUint32(end, true) !== 0x06054b50)
      end--;
    if (end < 0)
      throw new Error('Invalid ZIP directory');
    const count = v.getUint16(end + 10, true);
    let p = v.getUint32(end + 16, true), chosen = null;
    for (let i = 0; i < count; i++) {
      if (v.getUint32(p, true) !== 0x02014b50)
        throw new Error('Invalid ZIP entry');
      const flags = v.getUint16(p + 8, true), method = v.getUint16(p + 10, true),
            crc = v.getUint32(p + 16, true), compressed = v.getUint32(p + 20, true),
            size = v.getUint32(p + 24, true), n = v.getUint16(p + 28, true),
            extra = v.getUint16(p + 30, true), comment = v.getUint16(p + 32, true),
            mode = v.getUint32(p + 38, true) >>> 16, offset = v.getUint32(p + 42, true),
            entry = new TextDecoder().decode(bytes.subarray(p + 46, p + 46 + n));
      const ext = entry.match(/\.[^./\\]+$/)?.[0].toLowerCase();
      const matches = name === 'file_zip_extract_named'
                          ? entry === this.text(a[1])
                          : this.text(a[1]).toLowerCase().split('|').includes(ext);
      if (matches && !entry.endsWith('/') && !(flags & 1) && (mode & 0o170000) !== 0o120000 &&
          [ 0, 8 ].includes(method)) {
        chosen = {entry, ext, method, crc, compressed, size, offset};
        break;
      }
      p += 46 + n + extra + comment;
    }
    if (!chosen)
      throw new Error('No matching supported ZIP ROM');
    const c = chosen, limit = name === 'file_zip_extract_named' ? Math.min(a[3], 128 * 1024 * 1024)
                                                                : 128 * 1024 * 1024;
    if (c.size > limit)
      throw new Error('ZIP ROM size limit');
    const offset = c.offset;
    if (v.getUint32(offset, true) !== 0x04034b50)
      throw new Error('Invalid ZIP local entry');
    const start = offset + 30 + v.getUint16(offset + 26, true) + v.getUint16(offset + 28, true);
    if (start + c.compressed > bytes.length)
      throw new Error('Truncated ZIP');
    let data = bytes.slice(start, start + c.compressed);
    if (c.method === 8) {
      const stream =
          new Blob([ data ]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
      const reader = stream.getReader(), parts = [];
      let length = 0;
      for (;;) {
        const {done, value} = await reader.read();
        if (done)
          break;
        length += value.length;
        if (length > limit || length > c.size) {
          await reader.cancel();
          throw new Error('ZIP output size limit');
        }
        parts.push(value);
      }
      data = new Uint8Array(length);
      let at = 0;
      for (const part of parts) {
        data.set(part, at);
        at += part.length;
      }
    }
    let crc = 0xffffffff;
    for (const b of data) {
      crc ^= b;
      for (let bit = 0; bit < 8; bit++)
        crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    if (data.length !== c.size || ((crc ^ 0xffffffff) >>> 0) !== c.crc)
      throw new Error('ZIP CRC/size mismatch');
    let destination;
    if (name === 'file_zip_extract_named') {
      destination = this.path(this.text(a[2]));
      this.mem.put(a[4], 0);
    } else {
      const stem = this.text(a[3]);
      if (!/^[\w-]+$/.test(stem))
        throw new Error('Invalid ZIP cache name');
      destination = this.path(this.text(a[2]).replace(/\/$/, '') + '/' + stem + c.ext);
      if (new TextEncoder().encode(destination).length + 1 > a[5])
        throw new Error('ZIP output path does not fit');
    }
    this.FS.mkdirTree(destination.slice(0, destination.lastIndexOf('/')));
    this.FS.writeFile(destination + '.part', data);
    this.FS.rename(destination + '.part', destination);
    if (name === 'file_zip_extract_named')
      this.mem.put(a[4], data.length);
    else
      this.mem.string(a[4], a[5], destination);
    return 0;
  }
}
if (typeof module !== 'undefined')
  module.exports = {PaxBrowserHost};
