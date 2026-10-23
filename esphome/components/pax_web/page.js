"use strict";
// Incremental SHA-256 also works on a device's local HTTP page, where Web Crypto is unavailable.
class PaxSha256 {
  constructor(){this.h=new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);this.buffer=new Uint8Array(64);this.fill=0;this.bytes=0;this.w=new Uint32Array(64);}
  block(p){const K=PaxSha256.K,w=this.w,r=(x,n)=>(x>>>n)|(x<<(32-n));for(let i=0;i<16;i++)w[i]=(p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];for(let i=16;i<64;i++){let a=w[i-15],b=w[i-2];w[i]=(w[i-16]+(r(a,7)^r(a,18)^(a>>>3))+w[i-7]+(r(b,17)^r(b,19)^(b>>>10)))>>>0;}let [a,b,c,d,e,f,g,h]=this.h;for(let i=0;i<64;i++){const t1=(h+(r(e,6)^r(e,11)^r(e,25))+((e&f)^((~e)&g))+K[i]+w[i])>>>0,t2=((r(a,2)^r(a,13)^r(a,22))+((a&b)^(a&c)^(b&c)))>>>0;h=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0;}[a,b,c,d,e,f,g,h].forEach((v,i)=>this.h[i]=(this.h[i]+v)>>>0);}
  update(p){this.bytes+=p.length;let offset=0;while(offset<p.length){const n=Math.min(64-this.fill,p.length-offset);this.buffer.set(p.subarray(offset,offset+n),this.fill);this.fill+=n;offset+=n;if(this.fill===64){this.block(this.buffer);this.fill=0;}}}
  hex(){const bits=this.bytes*8;this.buffer[this.fill++]=128;if(this.fill>56){this.buffer.fill(0,this.fill);this.block(this.buffer);this.fill=0;}this.buffer.fill(0,this.fill);const v=new DataView(this.buffer.buffer);v.setUint32(56,Math.floor(bits/4294967296));v.setUint32(60,bits>>>0);this.block(this.buffer);return Array.from(this.h,x=>x.toString(16).padStart(8,"0")).join("");}
}
PaxSha256.K=new Uint32Array([0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
function paxTarget(folder,name){const target=folder.replace(/\/$/,"")+"/"+name;if(!/^\/(?:[A-Za-z0-9._-]+\/)*[A-Za-z0-9._-]+$/.test(target)||target.split("/").some(p=>p==="."||p==="..")||target.length>201)throw Error("Rename the file or change the destination: "+target);return "/sd"+target;}
if(typeof document!=="undefined"){
const $=id=>document.getElementById(id),token=document.querySelector('meta[name="pax-token"]').content;
let busy=false,chosen=[],queue=[];
const message=text=>$("status").textContent=text;
function lock(value){busy=value;$("progress").hidden=!value;if(value)$("progress").value=0;["starter","upload","close","files","folder","destination","replace","store-install"].forEach(id=>$(id).disabled=value);}
function preview(){queue=[];$("preview").replaceChildren();try{for(const file of chosen){if(file.size>1073741824)throw Error("File exceeds 1 GiB: "+file.name);const path=paxTarget($("destination").value,file.webkitRelativePath||file.name);queue.push({file,path});const li=document.createElement("li");li.textContent=path.slice(3)+" ("+file.size.toLocaleString()+" bytes)";$("preview").append(li);}const seen=new Set();for(const item of queue){const key=item.path.toLowerCase();if(seen.has(key))throw Error("Duplicate SD path: "+item.path);seen.add(key);}}catch(error){queue=[];message(error.message);}}
$("files").addEventListener("change",()=>{chosen=Array.from($("files").files);$("folder").value="";preview();});$("folder").addEventListener("change",()=>{chosen=Array.from($("folder").files);$("files").value="";preview();});$("destination").addEventListener("input",preview);
async function command(path){const response=await fetch(path,{method:"POST",headers:{"X-PAX-Token":token}});const text=await response.text();if(!response.ok)throw Error(text);return text;}
$("close").addEventListener("click",async()=>{try{message(await command("/pax/close"));}catch(error){message(error.message);}});
$("starter").addEventListener("click",async()=>{lock(true);$("progress").removeAttribute("value");try{message(await command("/pax/starter"));for(;;){await new Promise(resolve=>setTimeout(resolve,1000));const response=await fetch("/pax/status");if(!response.ok)throw Error("Device disconnected. Reopen its setup page.");const text=await response.text();message(text);const progress=response.headers.get("X-PAX-Progress");if(progress!==null)$("progress").value=Number(progress);if(!text.startsWith("Downloading"))break;}}catch(error){message(error.message);}finally{lock(false);$("progress").value=100;}});
async function digest(file,index){const hash=new PaxSha256();for(let offset=0;offset<file.size;offset+=262144){hash.update(new Uint8Array(await file.slice(offset,offset+262144).arrayBuffer()));message(`Checking ${index+1}/${queue.length}: ${file.name}`);await new Promise(resolve=>setTimeout(resolve,0));}return hash.hex();}
function send(item,sha,index){return new Promise((resolve,reject)=>{const xhr=new XMLHttpRequest();xhr.open("POST","/pax/upload");xhr.timeout=1800000;xhr.setRequestHeader("Content-Type","application/octet-stream");xhr.setRequestHeader("X-PAX-Token",token);xhr.setRequestHeader("X-PAX-Path",item.path);xhr.setRequestHeader("X-PAX-SHA256",sha);xhr.upload.onprogress=event=>{if(event.lengthComputable)$("progress").value=event.loaded/event.total*100;message(`Uploading ${index+1}/${queue.length}: ${item.path.slice(3)} (${event.loaded.toLocaleString()} bytes)`);};xhr.onload=()=>xhr.status===200?resolve(xhr.responseText):reject(Error(xhr.responseText||"Upload failed"));xhr.onerror=()=>reject(Error("Connection lost. The previous file is kept; retry the upload."));xhr.ontimeout=()=>reject(Error("Upload timed out. Retry when the device is available."));xhr.send(item.file);});}
let selectedApp=null,selectedFiles=[];
async function selectStoreApp(){
  const name=new URLSearchParams(location.hash.slice(1)).get('app');
  if(!name)return;
  $('store-app').hidden=false;
  try{
    if(!/^[A-Za-z0-9_-]+$/.test(name))throw Error('Invalid app selection');
    message('Loading selected app details');
    selectedApp=(await PaxStore.catalog()).find(app=>app.name===name);
    if(!selectedApp)throw Error('This app is no longer in the PAX Store');
    selectedFiles=await PaxStore.files(selectedApp);
    $('store-title').textContent='Install '+selectedApp.title;
    $('store-description').textContent=selectedApp.description.replace(/PAPP/g,'PAX').replace(/papp/g,'pax');
    for(const file of selectedFiles){const li=document.createElement('li');li.textContent=file.path.slice(3)+' ('+file.size.toLocaleString()+' bytes)';$('store-files').append(li);}
    const needed=(selectedApp.requires||[]).filter(item=>!item.download&&!item.optional).map(item=>item.path);
    $('store-requires').textContent=needed.length?'Additional files may be required: '+needed.join(', '):'Included downloadable data is installed with this app. Supply your own ROMs or game files where the description requires them.';
    message('Ready to install '+selectedApp.title);
  }catch(error){selectedApp=null;selectedFiles=[];message(error.message);$('store-install').disabled=true;}
}
$('store-install').addEventListener('click',async()=>{
  if(busy||!selectedApp)return;
  if(!$('store-replace').checked){message('Confirm replacement of matching SD files before installing.');return;}
  lock(true);$('progress').value=0;
  try{
    for(let i=0;i<selectedFiles.length;i++){
      const entry=selectedFiles[i];message('Downloading and checking '+entry.path);
      const {data,digest:sha}=await PaxStore.bytes(entry,async bytes=>{const hash=new PaxSha256();hash.update(bytes);return hash.hex();});
      queue=selectedFiles.map(file=>({path:file.path}));
      const item={path:entry.path,file:new Blob([data])};
      message(await send(item,sha,i));
      await new Promise(resolve=>setTimeout(resolve,100));
    }
    $('progress').value=100;message(selectedApp.title+' installed and verified. Choose STORAGE on the device. Check the app description for any additional game files.');
  }catch(error){message(error.message);}finally{lock(false);queue=[];}
});
selectStoreApp();
$("upload").addEventListener("click",async()=>{preview();if(queue.length===0){message("Choose valid files or a folder first.");return;}if(!$("replace").checked){message("Confirm replacement of existing paths before uploading.");return;}lock(true);try{for(let i=0;i<queue.length;i++){const sha=await digest(queue[i].file,i);message(await send(queue[i],sha,i));await new Promise(resolve=>setTimeout(resolve,100));}message(`Saved and verified ${queue.length} file(s). Choose STORAGE on the device.`);$("progress").value=100;}catch(error){message(error.message);}finally{lock(false);}});
}
