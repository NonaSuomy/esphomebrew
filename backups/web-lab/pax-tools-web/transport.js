/* HTTP request/response adapter. Browser fetch owns TLS and access policy. */
function attachPaxTransport(Module) {
  Module.paxSockets = {};
  let next = 1;
  const encoder = new TextEncoder();
  Module.paxConnect = (host, port, tls) => {
    if (!host || !port)
      return -1;
    const id = next++;
    Module.paxSockets[id] =
        {host, port, tls, request : new Uint8Array(), offset : 0, abort : new AbortController()};
    return id;
  };
  Module.paxSend = (id, bytes) => {
    const socket = Module.paxSockets[id];
    if (!socket || socket.error || socket.started ||
        socket.request.length + bytes.length > 8 * 1024 * 1024)
      return -1;
    const joined = new Uint8Array(socket.request.length + bytes.length);
    joined.set(socket.request);
    joined.set(bytes, socket.request.length);
    socket.request = joined;
    const text = new TextDecoder('latin1').decode(joined);
    const end = text.indexOf('\r\n\r\n');
    if (end < 0)
      return bytes.length;
    const lines = text.slice(0, end).split('\r\n');
    const [method, path] = lines.shift().split(' ');
    const bodySize = Number(lines.find(line => /^content-length:/i.test(line))?.split(':')[1] || 0);
    if (!Number.isSafeInteger(bodySize) || bodySize < 0 || joined.length < end + 4 + bodySize)
      return bytes.length;
    if (!['GET', 'HEAD', 'POST'].includes(method) || !path.startsWith('/')) {
      socket.error = true;
      return -1;
    }
    socket.started = true;
    const headers = {};
    for (const line of lines) {
      const colon = line.indexOf(':');
      if (colon < 0)
        continue;
      const name = line.slice(0, colon).trim();
      if ([ 'accept', 'content-type', 'range', 'if-modified-since', 'if-none-match' ].includes(
              name.toLowerCase()))
        headers[name] = line.slice(colon + 1).trim();
    }
    const url = `${socket.tls ? 'https' : 'http'}://${socket.host}:${socket.port}${path}`;
    const options = {method, headers, credentials : 'omit', signal : socket.abort.signal};
    if (method === 'POST')
      options.body = joined.slice(end + 4, end + 4 + bodySize);
    fetch(url, options)
        .then(async response => {
          const body = new Uint8Array(await response.arrayBuffer());
          if (body.length > 64 * 1024 * 1024)
            throw new Error('HTTP response exceeds 64 MiB');
          let head = `HTTP/1.1 ${response.status} ${response.statusText}\r\nContent-Length: ${
              body.length}\r\nConnection: close\r\n`;
          for (const name of ['content-type', 'last-modified', 'etag', 'location', 'date']) {
            const value = response.headers.get(name);
            if (value)
              head += `${name}: ${value}\r\n`;
          }
          const prefix = encoder.encode(head + '\r\n');
          socket.response = new Uint8Array(prefix.length + body.length);
          socket.response.set(prefix);
          socket.response.set(body, prefix.length);
        })
        .catch(error => {
          socket.error = true;
          Module.printErr(`Browser HTTP failed: ${url}: ${
              error.message}. Check CORS and mixed-content permissions.`);
        });
    return bytes.length;
  };
  Module.paxRecv = (id, pointer, size, tls, heap) => {
    const socket = Module.paxSockets[id];
    if (!socket || socket.error)
      return -1;
    if (!socket.response)
      return tls ? 0 : -2;
    const length = Math.min(size, socket.response.length - socket.offset);
    if (!length)
      return tls ? -1 : 0;
    heap.set(socket.response.subarray(socket.offset, socket.offset + length), pointer);
    socket.offset += length;
    return length;
  };
}
