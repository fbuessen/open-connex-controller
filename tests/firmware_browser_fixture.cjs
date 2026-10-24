// Loopback-only simulated HTTP backend for production-asset browser tests.
// This does not stand in for device HTTP, NVS, crypto timing, or radio acceptance.
const {createServer} = require('node:http');
const {readFileSync} = require('node:fs');
const {join} = require('node:path');
function fixture(root) {
  const state = {configured:false,authenticated:false,password:'',portal:false,token:'',icons:{},status:'configured',scanning:false,failWifi:false,error:'',dropPatch:false,calls:[],zones:[
    {id:'zone_0000000000000001',name:'Living room',configuration:{target_temperature:21,unit:'celsius',mode:'normal',eco_offset:-3}},
    {id:'zone_0000000000000002',name:'Bedroom',configuration:{target_temperature:19,unit:'celsius',mode:'eco',eco_offset:-2}},
    {id:'zone_0000000000000003',name:'Office',configuration:{target_temperature:20.5,unit:'celsius',mode:'normal',eco_offset:-3}}
  ]};
  const server = createServer(async(req,res)=>{
    const url=req.url.split('?')[0], method=req.method;
    res.setHeader('Cache-Control','no-store');
    res.setHeader('Connection','close'); // Match firmware's one-request connections.
    res.setHeader('Content-Security-Policy',"default-src 'self'; script-src 'self'; style-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    const reply=(status,value)=>{res.writeHead(status,{'Content-Type':'application/json'});res.end(value===undefined?'':JSON.stringify(value));};
    if (['/','/app.js','/style.css','/favicon.svg'].includes(url)) {
      res.setHeader('Content-Type',url==='/favicon.svg'?'image/svg+xml':url==='/app.js'?'application/javascript':url==='/style.css'?'text/css':'text/html');
      res.end(readFileSync(join(root,url==='/'?'index.html':url.slice(1))));return;
    }
    const authenticated=state.authenticated && (req.headers.cookie||'').includes('connex_session=test-session');
    let body='';for await(const chunk of req)body+=chunk;
    const value=body?JSON.parse(body):{};state.calls.push({url,method,value,at:Date.now()});
    if(method!=='GET' && req.headers['x-connex-nonce']!=='nonce') return reply(403,{error:{message:'Missing nonce'}});
    if(url==='/ui/session') return reply(200,{configured:state.configured,authenticated,portal:state.portal,nonce:'nonce',enrollment_allowed:state.portal&&!state.configured});
    if(url==='/ui/enroll'||url==='/ui/login') {
      if(state.authDelay)await new Promise(done=>setTimeout(done,state.authDelay));
      if(url==='/ui/enroll') {if(state.configured)return reply(409,{error:{message:'Already configured'}});if(!state.portal)return reply(403,{error:{message:'Setup required'}});state.configured=true;state.password=value.password;}
      else if(value.password!==state.password)return reply(401,{error:{message:'Incorrect password'}});
      state.authenticated=true;res.setHeader('Set-Cookie','connex_session=test-session; Path=/; HttpOnly; SameSite=Strict');return reply(200,{authenticated:true});
    }
    if(!authenticated && !(state.portal && url.startsWith('/ui/network')))return reply(401,{error:{message:'Sign in'}});
    if(url==='/ui/logout') {state.authenticated=false;res.setHeader('Set-Cookie','connex_session=; Max-Age=0; Path=/');return reply(204);}
    if(url==='/ui/password') {
      if(state.authDelay)await new Promise(done=>setTimeout(done,state.authDelay));
      if(value.current!==state.password)return reply(401,{error:{message:'Incorrect current password'}});
      state.password=value.password;state.authenticated=false;res.setHeader('Set-Cookie','connex_session=; Max-Age=0; Path=/');return reply(204);
    }
    if(url==='/ui/token') {
      if(method==='GET' && state.holdTokenRead) {
        const token=state.token;await new Promise(done=>{state.releaseTokenRead=done;});state.holdTokenRead=false;
        return reply(200,{token});
      }
      if(method==='POST')state.token=state.token==='a'.repeat(64)?'b'.repeat(64):'a'.repeat(64);if(method==='DELETE')state.token='';return reply(200,{token:state.token});
    }
    if(url==='/ui/icons') {if(method==='PATCH')state.icons[value.id]=value.icon;return reply(200,{icons:state.icons});}
    if(url==='/ui/api/v1/device')return reply(200,{id:'controller_test',firmware_version:'0.2.6',health:'ok'});
    if(url==='/ui/api/v1/zones') {
      if(method==='POST') {
        if(state.zones.length>=8)return reply(409,{error:{message:'Zone capacity reached'}});
        const number=state.nextZoneId||4;state.nextZoneId=number+1;
        const zone={id:'zone_'+number.toString(16).padStart(16,'0'),name:value.name,configuration:{target_temperature:20,unit:'celsius',mode:'normal',eco_offset:0}};
        state.zones.push(zone);return reply(201,zone);
      }
      if(state.holdZonesRead) {await new Promise(done=>{state.releaseZonesRead=done;});state.holdZonesRead=false;}
      return reply(200,{zones:state.zones});
    }
    if(url.startsWith('/ui/api/v1/zones/')) {
      const id=url.split('/')[5],zone=state.zones.find(z=>z.id===id);
      if(url.endsWith('/pairing')) {
        if(method==='POST' && zone.configuration.target_temperature===null)return reply(422,{error:{message:'Invalid or unsupported field value'}});
        return reply(method==='POST'?202:204);
      }
      if(method==='DELETE') {state.zones=state.zones.filter(z=>z.id!==id);return reply(204);}
      if(method==='PATCH') {
        for(const [key,val]of Object.entries(value)) if(key==='name')zone.name=val;else zone.configuration[key]=val;
        // Commit but withhold the response until the client aborts. Destroying a
        // reused test socket before headers can trigger Chromium transport retry.
        if(state.dropPatch) {state.dropPatch=false;return;}
        return reply(Object.keys(value).some(k=>k!=='name')?202:200,zone);
      }
      return reply(200,zone);
    }
    if(url==='/ui/network/scan') {state.scanning=true;setTimeout(()=>{state.scanning=false;},100);return reply(202);}
    if(url==='/ui/networks')return reply(200,{scanning:state.scanning,networks:[{ssid:'Home & <guest>',rssi:-42,secured:true}]});
    if(url==='/ui/network') {
      if(method==='POST') {state.status='connecting';setTimeout(()=>{state.status=state.failWifi?'setup':state.portal?'saved':'configured';state.error=state.failWifi?'Connection failed; try again':'';},100);return reply(202);}
      return reply(200,{state:state.status,nonce:'nonce',ssid:'Home & <guest>',connected:true,error:state.error,scanning:state.scanning,hostname:'open-connex',remaining_seconds:0});
    }
    reply(404,{error:{message:'Unknown route '+url}});
  });
  return {server,state};
}
module.exports=fixture;
