/* Contract checks for the browser HTTP service adapter. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(__dirname + '/transport.js', 'utf8');
let calls = [];
const context = {
  TextEncoder,
  TextDecoder,
  AbortController,
  fetch : async (url, options) => {
    calls.push({url, options});
    return {
      status : 200,
      statusText : 'OK',
      headers : new Headers({'content-type' : 'text/html'}),
      arrayBuffer : async () => new TextEncoder().encode('PAX HTTP test').buffer
    };
  }
};
vm.createContext(context);
vm.runInContext(source, context);
const moduleHost = {
  printErr : () => {}
};
context.attachPaxTransport(moduleHost);
const encoder = new TextEncoder();
const heap = new Uint8Array(1024);
(async () => {
  const h = moduleHost.paxConnect('example.org', 443, true);
  assert.equal(moduleHost.paxRecv(h, 0, 1024, true, heap), 0);
  const first = encoder.encode('GET /page HTTP/1.1\r\nHost: example.org\r\nAccept: text/html\r\n');
  assert.equal(moduleHost.paxSend(h, first), first.length);
  assert.equal(calls.length, 0);
  moduleHost.paxSend(h, encoder.encode('\r\n'));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(calls[0].url, 'https://example.org:443/page');
  assert.equal(calls[0].options.credentials, 'omit');
  let n = moduleHost.paxRecv(h, 0, heap.length, true, heap);
  const text = new TextDecoder().decode(heap.slice(0, n));
  assert.match(text, /Content-Length: 13\r\n/);
  assert.ok(text.endsWith('PAX HTTP test'));
  assert.equal(moduleHost.paxRecv(h, 0, 1024, true, heap), -1);
  const post = moduleHost.paxConnect('localhost', 8767, false);
  moduleHost.paxSend(post, encoder.encode('POST /send HTTP/1.1\r\nContent-Length: 3\r\n\r\na'));
  assert.equal(calls.length, 1);
  moduleHost.paxSend(post, encoder.encode('bc'));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(new TextDecoder().decode(calls[1].options.body), 'abc');
  const bad = moduleHost.paxConnect('localhost', 8767, false);
  assert.equal(moduleHost.paxSend(bad, encoder.encode('CONNECT remote:443 HTTP/1.1\r\n\r\n')), -1);
  context.fetch = async () => { throw new Error('CORS failure'); };
  const failed = moduleHost.paxConnect('example.org', 443, true);
  moduleHost.paxSend(failed, encoder.encode('GET / HTTP/1.1\r\n\r\n'));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(moduleHost.paxRecv(failed, 0, 100, true, heap), -1);
  console.log(
      'Browser HTTP adapter: split headers/body, GET/POST, TLS EOF, invalid method and fetch failure passed');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
