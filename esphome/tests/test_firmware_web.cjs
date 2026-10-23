const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=fs.readFileSync(process.argv[2]||require('node:path').join(__dirname,'../components/pax_web/page.js'),'utf8');
async function main(){
 const nodes=new Map(),handlers={},timers=[],requests=[];
 const node=id=>{if(!nodes.has(id))nodes.set(id,{addEventListener(event,fn){handlers[id+':'+event]=fn;},removeAttribute(){}});return nodes.get(id);};
 let firmware={state:2,profile:'nano-lcd43',installed_build:'9007199254740993',latest_build:'9007199254740993',message:'No newer update'};
 let c6={state:0,profile:'c6-sdio',installed_version:'',recommended_version:'2.12.12',message:'Check C6'};
 let confirmed=false;
 const context={document:{getElementById:node,querySelector:()=>({content:'test'})},location:{hash:''},URLSearchParams,Uint32Array,DataView,
  setTimeout:fn=>{timers.push(fn);return timers.length;},clearTimeout:()=>{},confirm:()=>confirmed,
  fetch:async(url,options={})=>{
   requests.push({url,...options});
   if(options.method==='POST'){
    if(url==='/pax/firmware/check')firmware={...firmware,state:1};
    if(url==='/pax/firmware/install')firmware={...firmware,state:4};
    if(url==='/pax/c6/check')c6={...c6,state:1};
    if(url==='/pax/c6/install')c6={...c6,state:3};
    return {ok:true,text:async()=> 'Accepted'};
   }
   return {ok:true,json:async()=>({...url==='/pax/firmware'?firmware:c6})};
  }};
 vm.runInNewContext(source,context);
 const flush=()=>new Promise(resolve=>setImmediate(resolve));await flush();
 assert.match(node('firmware-status').textContent,/Installed version: build 9007199254740993/);
 assert.match(node('firmware-status').textContent,/Latest published version: build 9007199254740993/);
 assert.match(node('firmware-status').textContent,/You are on the latest version/);
 assert.equal(node('firmware-install').disabled,true);
 await handlers['firmware-check:click']();assert.equal(node('firmware-check').disabled,true);
 firmware={...firmware,state:3,latest_build:'9007199254740994',message:'Update available'};
 await timers.shift()();await flush();assert.equal(node('firmware-install').disabled,false);
 assert.match(node('firmware-status').textContent,/Latest published version: build 9007199254740994/);
 assert.doesNotMatch(node('firmware-status').textContent,/You are on the latest version/);
 let count=requests.length;await handlers['firmware-install:click']();assert.equal(requests.length,count);
 confirmed=true;await handlers['firmware-install:click']();
 const install=requests.find(x=>x.url==='/pax/firmware/install');assert.equal(install.headers['X-PAX-Build'],'9007199254740994');
 await handlers['c6-check:click']();c6={...c6,state:2,installed_version:'2.12.3',message:'Matching image verified'};
 await timers.pop()();await flush();assert.equal(node('c6-install').disabled,false);
 assert.match(node('c6-status').textContent,/Installed version: 2.12.3/);
 assert.match(node('c6-status').textContent,/Board-recommended version: 2.12.12/);
 await handlers['c6-install:click']();assert.equal(requests.find(x=>x.url==='/pax/c6/install').headers['X-PAX-C6'],'c6-sdio');
 console.log('Installed/latest versions, precise 64-bit IDs, check polling and explicit P4/C6 confirmations passed');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
