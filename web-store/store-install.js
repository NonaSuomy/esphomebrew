"use strict";
// Shared by the public store and the device page. URLs are fixed to our public catalog.
const PaxStore = (() => {
  const base = "https://nonasuomy.github.io/esphomebrew/";
  const encoder = new TextEncoder();
  function target(value) {
    if (!/^\/sd\/(?:[A-Za-z0-9._-]+\/)*[A-Za-z0-9._-]+$/.test(value) || value.length > 204 || value.split('/').some(p=>p==='.'||p==='..')) throw Error('Invalid SD path');
    return value;
  }
  function publicURL(value) {
    const url = new URL(value, base);
    if (url.origin !== new URL(base).origin || !url.pathname.startsWith('/esphomebrew/') || url.search || url.hash) throw Error('Invalid catalog download URL');
    return url.href;
  }
  async function catalog() {
    const response = await fetch(base + 'store.json', {cache:'no-cache'});
    if (!response.ok) throw Error('Could not load the app catalog');
    const data = await response.json();
    if (!Array.isArray(data.apps)) throw Error('Invalid app catalog');
    return data.apps;
  }
  async function files(app) {
    if (!/^[A-Za-z0-9_-]+$/.test(app.name) || !/^[a-f0-9]{64}$/.test(app.sha256)) throw Error('Invalid app metadata');
    const response = await fetch(publicURL(app.info_url), {cache:'no-cache'});
    if (!response.ok) throw Error('Could not load app details');
    const info = await response.json();
    if (info.name !== app.name || info.sha256 !== app.sha256 || info.size !== app.size || info.version !== app.version) throw Error('App details changed. Reload the store.');
    const sidecar = encoder.encode(JSON.stringify(info));
    return [
      ...(app.data?.files || []).map(file=>({path:target('/sd/'+file.target),url:publicURL(file.url),size:file.size,sha256:file.sha256})),
      {path:target('/sd/roms/pax/'+app.name+'.pax'),url:publicURL(app.url),size:app.size,sha256:app.sha256},
      // Details follow the binary so a failed binary transfer keeps the old listing.
      {path:target('/sd/roms/pax/'+app.name+'.json'),bytes:sidecar,size:sidecar.length}
    ];
  }
  async function bytes(file, hash) {
    let data = file.bytes;
    if (!data) {
      if (!Number.isInteger(file.size) || file.size < 0 || file.size > 1073741824 || !/^[a-f0-9]{64}$/.test(file.sha256)) throw Error('Invalid file metadata');
      const response = await fetch(file.url);
      if (!response.ok) throw Error('Download failed: '+file.path);
      const chunks = []; let size = 0;
      const reader = response.body.getReader();
      for (;;) {
        const {done,value} = await reader.read(); if (done) break;
        size += value.length;
        if (size > file.size) {await reader.cancel(); throw Error('Download exceeds expected size');}
        chunks.push(value);
      }
      if (size !== file.size) throw Error('Incomplete download: '+file.path);
      data = new Uint8Array(size); let offset=0;
      for (const chunk of chunks) {data.set(chunk,offset); offset+=chunk.length;}
    }
    const digest = await hash(data);
    if (file.sha256 && digest !== file.sha256) throw Error('SHA-256 check failed: '+file.path);
    return {data,digest};
  }
  function frame(command, payload=new Uint8Array()) {
    if (payload.length>254) throw Error('Serial payload too large');
    const out=new Uint8Array(11+payload.length);
    out.set(encoder.encode('IMPROV')); out[6]=1; out[7]=3; out[8]=payload.length+1; out[9]=command; out.set(payload,10);
    out[out.length-1]=out.subarray(0,-1).reduce((sum,v)=>(sum+v)&255,0);
    return out;
  }
  // Logs and split frames may share the serial stream. Only valid vendor replies are accepted.
  class Decoder {
    constructor(){this.buffer=[];}
    push(bytes) {
      this.buffer.push(...bytes); const replies=[];
      while(this.buffer.length>=10) {
        if (!encoder.encode('IMPROV').every((v,i)=>this.buffer[i]===v) || this.buffer[6]!==1 || this.buffer[7]!==4) {this.buffer.shift();continue;}
        const length=this.buffer[8],total=10+length;
        if (this.buffer.length<total) break;
        const packet=this.buffer.slice(0,total);
        if (length>=2 && packet.slice(0,-1).reduce((sum,v)=>(sum+v)&255,0)===packet.at(-1)) {
          replies.push({command:packet[9],status:packet[10],message:new TextDecoder().decode(new Uint8Array(packet.slice(11,-1)))});
          this.buffer.splice(0,total);
        } else this.buffer.shift();
      }
      return replies;
    }
  }
  class Serial {
    async connect() {
      if (!navigator.serial) throw Error('Use desktop Chrome or Edge for USB serial installation.');
      this.port=await navigator.serial.requestPort();
      try {
        await this.port.open({baudRate:115200});
        await this.port.setSignals({dataTerminalReady:false,requestToSend:false});
        this.writer=this.port.writable.getWriter(); this.reader=this.port.readable.getReader();
        this.decoder=new Decoder(); this.running=true; this.reading=this.readLoop();
        // Some USB bridges reset the board when the port opens. Let setup finish.
        const deadline=Date.now()+45000;let reply;
        while(Date.now()<deadline){
          try{reply=await this.rpc(0x74);if(reply.message==='PAXSD1')break;}catch{}
          await new Promise(resolve=>setTimeout(resolve,500));
        }
        if(reply?.message!=='PAXSD1') throw Error('No PAX serial SD response. Update PAX firmware and use the programming USB port.');
      } catch(error) {await this.disconnect();throw error;}
    }
    async readLoop() {
      try {while(this.running){const {value,done}=await this.reader.read();if(done)break;for(const reply of this.decoder.push(value)){if(this.pending&&reply.command===this.pending.command&&this.pending.match(reply)){this.pending.resolve(reply);this.pending=null;}}}}
      catch(error){if(this.pending)this.pending.reject(error);}
      finally {if(this.pending){this.pending.reject(Error('Serial device disconnected'));this.pending=null;}}
    }
    async rpc(command,payload=new Uint8Array(),match=()=>true) {
      if(this.pending) throw Error('Serial command already in progress');
      let timer;
      const response=new Promise((resolve,reject)=>{this.pending={command,resolve,reject,match};timer=setTimeout(()=>{this.pending=null;reject(Error('Serial response timed out. Update PAX firmware or reconnect the device.'));},5000);});
      try {await this.writer.write(frame(command,payload));return await response;}
      finally {clearTimeout(timer);this.pending=null;}
    }
    async send(path,data,digest,progress=()=>{}) {
      target(path);const name=encoder.encode(path),header=new Uint8Array(36+name.length);
      new DataView(header.buffer).setUint32(0,data.length,true);
      header.set(digest.match(/../g).map(v=>parseInt(v,16)),4);header.set(name,36);
      let reply=await this.rpc(0x70,header);if(reply.status!==0)throw Error(reply.message);
      try {
        let sequence=0;
        for(let offset=0;offset<data.length;offset+=240){
          const chunk=data.subarray(offset,offset+240),payload=new Uint8Array(4+chunk.length);
          new DataView(payload.buffer).setUint32(0,sequence,true);payload.set(chunk,4);
          let attempts=0;
          let retries=0;
          do {
            try {reply=await this.rpc(0x71,payload,r=>r.status===1||r.message===`Accepted ${sequence}`||r.message===`Retry ${sequence}`);}
            catch(error){if(++retries>3)throw error;reply={status:2};}
            if(reply.status===2){if(++attempts>100)throw Error('Device storage stayed busy');await new Promise(r=>setTimeout(r,20));}
          } while(reply.status===2);
          if(reply.status!==0)throw Error(reply.message);
          sequence++;progress(Math.min(offset+chunk.length,data.length),data.length);
        }
        for(let attempt=0;attempt<120;attempt++){
          reply=await this.rpc(0x72);if(reply.status!==2)break;
          await new Promise(r=>setTimeout(r,250));
        }
        if(reply.status!==0)throw Error(reply.message);
      } catch(error){try{await this.rpc(0x73);}catch{}throw error;}
    }
    async disconnect(){this.running=false;try{await this.reader?.cancel();await this.reading;}catch{}try{this.reader?.releaseLock();this.writer?.releaseLock();await this.port?.close();}catch{}this.port=null;}
  }
  function deviceURL(value,app) {
    const url=new URL(value.includes('://')?value:'http://'+value);
    if(url.protocol!=='http:' || url.username || url.password || !(url.hostname.endsWith('.local') || /^192\.168\.\d+\.\d+$/.test(url.hostname) || /^10\.\d+\.\d+\.\d+$/.test(url.hostname) || /^172\.(1[6-9]|2\d|3[01])\.\d+\.\d+$/.test(url.hostname))) throw Error('Enter the device local IP address or .local name.');
    if(!url.port)url.port='8080';url.pathname='/pax';url.search='';url.hash='app='+encodeURIComponent(app);return url.href;
  }
  return {catalog,files,bytes,frame,Decoder,Serial,deviceURL};
})();
if(typeof module!=='undefined')module.exports=PaxStore;
if(typeof document!=='undefined' && document.getElementById('store-connection')) {
  const $=id=>document.getElementById(id),status=text=>$('install-status').textContent=text;
  let serial=null,busy=false,apps;
  function lock(value){busy=value;document.querySelectorAll('.install-app,#serial-connect,#serial-disconnect,#close-app').forEach(button=>button.disabled=value);}
  $('serial-connect').addEventListener('click',async()=>{lock(true);try{serial=new PaxStore.Serial();await serial.connect();$('serial-disconnect').hidden=false;status('USB serial connected. Select Install on device beside an app.');}catch(error){serial=null;status(error.message);}finally{lock(false);}});
  $('serial-disconnect').addEventListener('click',async()=>{if(busy)return;await serial?.disconnect();serial=null;$('serial-disconnect').hidden=true;status('Serial disconnected');});
  $('close-app').addEventListener('click',async()=>{if(!serial){status('Connect USB serial first, or close the app on your device.');return;}try{status((await serial.rpc(0x75)).message);}catch(error){status(error.message);}});
  document.querySelectorAll('.install-app').forEach(button=>button.addEventListener('click',async()=>{
    if(busy)return;
    if(!serial){try{const url=PaxStore.deviceURL($('device-address').value,button.dataset.app);window.open(url,'_blank','noopener');status('Continue on the device page to confirm the Wi-Fi installation.');}catch(error){status(error.message);}return;}
    if(!$('install-replace').checked){status('Confirm replacement of matching SD files before installing.');return;}
    lock(true);$('install-progress').value=0;
    try {
      apps=await PaxStore.catalog();const app=apps.find(a=>a.name===button.dataset.app);if(!app)throw Error('App is no longer in the catalog');
      const files=await PaxStore.files(app);
      for(let i=0;i<files.length;i++){
        const file=files[i];status('Downloading and checking '+file.path);
        const {data,digest}=await PaxStore.bytes(file,async bytes=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),v=>v.toString(16).padStart(2,'0')).join(''));
        await serial.send(file.path,data,digest,(done,total)=>{$('install-progress').value=((i+(total?done/total:1))/files.length)*100;status('USB transfer '+file.path+' ('+done.toLocaleString()+' / '+total.toLocaleString()+' bytes)');});
      }
      $('install-progress').value=100;status(app.title+' installed and verified. Choose STORAGE on the device.');
    }catch(error){status(error.message);}finally{lock(false);}
  }));
}
