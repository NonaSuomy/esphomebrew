"""Live UDP discovery responds only to versioned requests and stops cleanly."""
from pathlib import Path
import importlib.util,socket,struct,unittest
spec=importlib.util.spec_from_file_location('pax_supply_server',Path(__file__).resolve().parents[1]/'tools/serve_dev.py')
server=importlib.util.module_from_spec(spec);spec.loader.exec_module(server)
class SupplyServer(unittest.TestCase):
 def test_query_reply_custom_http_port_and_malformed_requests(self):
  discovery=server.SupplyDiscovery(8123,'127.0.0.1',0)
  peer=discovery.socket.getsockname();client=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);client.settimeout(.15)
  try:
   for malformed in (b'',b'PAXFIND0abcd',b'PAXFIND1abc',b'PAXFIND1abcde'):
    client.sendto(malformed,peer)
    with self.assertRaises(socket.timeout):client.recvfrom(64)
   nonce=b'\x01\x02\x03\x04';client.sendto(server.DISCOVERY_QUERY+nonce,peer)
   data,source=client.recvfrom(64)
   self.assertEqual(source,peer);self.assertEqual(data,server.DISCOVERY_REPLY+nonce+struct.pack('<H',8123))
  finally:client.close();discovery.close()
  self.assertFalse(discovery.thread.is_alive())
if __name__=='__main__':unittest.main()
