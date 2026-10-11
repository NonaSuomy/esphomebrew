const assert=require('node:assert/strict');
const crypto=require('node:crypto');
const store=require('../web-store/store-install.js');
const hash=async bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
function reply(command,status,text){const p=Buffer.concat([Buffer.from('IMPROV'),Buffer.from([1,4,2+text.length,command,status]),Buffer.from(text)]);return Buffer.concat([p,Buffer.from([p.reduce((s,v)=>(s+v)&255,0)])]);}
(async()=>{
 const data=Buffer.from('checked package');const app={name:'test',version:'1',size:data.length,sha256:await hash(data),url:'https://nonasuomy.github.io/esphomebrew/test.pax',info_url:'https://nonasuomy.github.io/esphomebrew/test.json'};
 const frame=store.frame(0x71,new Uint8Array([1,2,3]));assert.equal(frame[8],4);assert.equal(frame.at(-1),frame.subarray(0,-1).reduce((s,v)=>(s+v)&255,0));
 const parser=new store.Decoder(),good=reply(0x74,0,'PAXSD1'),bad=Buffer.from(good);bad[12]^=1;
 const stream=Buffer.concat([Buffer.from('boot log\n'),bad,Buffer.from('more logs'),good,good]);let got=[];for(let i=0;i<stream.length;i+=3)got.push(...parser.push(stream.subarray(i,i+3)));assert.equal(got.length,2);assert.equal(got[0].message,'PAXSD1');
 const originalFetch=global.fetch;
 global.fetch=async url=>url.endsWith('.json')?{ok:true,json:async()=>app}:{ok:true,body:{getReader:()=>{let sent=false;return {read:async()=>sent?{done:true}:(sent=true,{done:false,value:data}),cancel:async()=>{}};}}};
 const files=await store.files(app);assert.deepEqual(files.map(f=>f.path),['/sd/roms/pax/test.pax','/sd/roms/pax/test.json']);assert.equal((await store.bytes(files[0],hash)).digest,app.sha256);
 await assert.rejects(()=>store.bytes({...files[0],sha256:'0'.repeat(64)},hash),/SHA-256/);
 await assert.rejects(()=>store.bytes({...files[0],size:1},hash),/expected size/);
 await assert.rejects(()=>store.files({...app,info_url:'https://evil.example/a.json'}),/URL/);
 await assert.rejects(()=>store.files({...app,name:'../outside'}),/metadata/);
 global.fetch=async()=>({ok:true,json:async()=>({...app,sha256:'0'.repeat(64)})});await assert.rejects(()=>store.files(app),/changed/);
 global.fetch=originalFetch;
 assert.equal(store.deviceURL('192.168.0.241','doom'),'http://192.168.0.241:8080/pax#app=doom');
 for(const value of ['https://192.168.0.241','example.com','http://user:pass@192.168.0.241','http://127.0.0.1'])assert.throws(()=>store.deviceURL(value,'doom'));
 let commands=[],serial=new store.Serial();serial.rpc=async(cmd,payload)=>{commands.push(cmd);return {status:0,message:'ok'};};await serial.send('/sd/test.pax',new Uint8Array(481),'ab'.repeat(32));assert.deepEqual(commands,[0x70,0x71,0x71,0x71,0x72]);
 let retryCount=0;serial.rpc=async cmd=>{if(cmd===0x71&&retryCount++===0)throw Error('dropped reply');return {status:0,message:'ok'};};await serial.send('/sd/retry.pax',new Uint8Array(241),'ab'.repeat(32));assert.equal(retryCount,3);
 serial.rpc=async cmd=>({status:cmd===0x72?1:0,message:'Hash mismatch'});await assert.rejects(()=>serial.send('/sd/test.pax',new Uint8Array(1),'ab'.repeat(32)),/Hash mismatch/);
 console.log('Serial framing, fragmented/logged/corrupt replies, transfer completion, catalog integrity, hashes, sizes and local Wi-Fi handoff checks passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
