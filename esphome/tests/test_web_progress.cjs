const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../components/pax_web/page.js'),'utf8');
async function check(failure){
 const nodes=new Map(),handlers={};
 const node=id=>{if(!nodes.has(id))nodes.set(id,{hidden:true,value:0,disabled:false,addEventListener(event,fn){handlers[id+':'+event]=fn;},removeAttribute(key){delete this[key];}});return nodes.get(id);};
 let calls=0;
 const context={document:{getElementById:node,querySelector:()=>({content:'test'})},location:{hash:''},URLSearchParams,Uint32Array,DataView,setTimeout:fn=>fn(),clearTimeout:()=>{},fetch:async(url)=>{
  if(url==="/pax/c6")return {ok:true,json:async()=>({state:0,profile:"",message:"Unsupported"})};
  if(url==="/pax/firmware")return {ok:true,json:async()=>({state:0,profile:"nano-lcd43",installed_build:"0",latest_build:"0",message:"Ready"})};
  assert.equal(node('progress').hidden,false);
  calls++;if(failure && calls===2)throw Error('disconnected');
  return {ok:true,text:async()=>calls===1?'Downloading verified SD starter files':'SD starter installed and verified.',headers:{get:()=>null}};
 }};
 vm.runInNewContext(source,context);
 await handlers['starter:click']();
 assert.equal(calls,2);assert.equal(node('progress').hidden,true);assert.equal(node('starter').disabled,false);
 assert.match(node('status').textContent,failure?/disconnected/:/installed and verified/);
}
(async()=>{await check(false);await check(true);console.log('Starter progress hides on completion and error while status remains visible');})().catch(e=>{console.error(e);process.exitCode=1;});
