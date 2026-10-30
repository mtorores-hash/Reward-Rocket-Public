const fs=require('fs'),vm=require('vm'),assert=require('assert/strict');
const functions=['rr342CycleActive','rr342NormalCompletion','rr342AudioTailActive','rr342DropEtaMs','rr342CountdownCount','audioTargetFromStatus','planAudioCountdown','beginAudioCycle','observeAudioStatus','cancelAudioCycle','interruptAudio'];
function harness(file){
 const source=fs.readFileSync(file,'utf8');let now=0,next=0,events=[],timers=new Map();
 const ctx={Math,Number,Set,Promise,performance:{now:()=>now},document:{hidden:false},setTimeout:(f,ms)=>{timers.set(++next,{f,at:now+ms});return next},clearTimeout:id=>timers.delete(id)};
 vm.createContext(ctx);
 vm.runInContext(`const RR342_INTRO_MS=2611,RR342_BLASTOFF_MS=5000,RR342_ASSETS=['intro','ten','nine','eight','seven','six','five','four','three','two','one','launch','blastoff'];
let audioPrefs={enabled:true,volume:.8,offsetMs:0},audioBaseline=false,lastCycleActive=false,lastRewardCycleId=null,lastServerMs=null,serverAnchorPerf=0,serverAnchorMs=0;
let audioContext={state:'running'},audioBuffers=Object.fromEntries(RR342_ASSETS.map(n=>[n,{duration:n==='launch'?.5894375:n==='blastoff'?5:.45}])),audioCycle=null,audioPollTimer=0,audioNodes=new Set();
function stopAudioNodes(){};function playAudio(name,at){record(name,at);return {}};
`+functions.map(n=>{const f=source.split(/\r?\n/).find(l=>l.startsWith('function '+n+'('));assert(f,n);return f}).join('\n')+'\nthis.api={observeAudioStatus,rr342DropEtaMs,interruptAudio};',Object.assign(ctx,{record:(name,at)=>events.push({name,at,now})}));
 function advance(to){let guard=0;while(true){const pair=[...timers].sort((a,b)=>a[1].at-b[1].at)[0];if(!pair||pair[1].at>to)break;assert(++guard<10000);now=pair[1].at;timers.delete(pair[0]);pair[1].f()}now=to}
 return {api:ctx.api,events,advance,status(s,t){advance(t);ctx.api.observeAudioStatus(s,t-20,t)},suspend(){vm.runInContext("audioContext.state='suspended'",ctx)},resume(){vm.runInContext("audioContext.state='running'",ctx)}};
}
const idle=(id=0)=>({nonce:'boot1',mode:'Locked',credit:'None',run:'Idle',reward_in_progress:false,storage_good:true,position_valid:true,position_steps:-8000,pickup_steps:-8000,dropoff_steps:0,dropoff_taught:true,step_interval_us:3000,reward_cycle_id:id,server_ms:0});
function run(file,cycles=1){const h=harness(file);h.status(idle(),0);for(let id=1;id<=cycles;id++){
 const start=100+(id-1)*31000,arrival=start+24000;
 for(let t=start;t<=arrival+6000;t+=250){const moving=t<arrival,returning=t>=arrival+500;let s={...idle(id),server_ms:t-10,mode:'Busy',credit:'Consumed',reward_in_progress:true,position_valid:!moving,position_steps:moving?-Math.round((arrival-t)/3):0,run:moving?'SeekHall':returning?'RewardToPickup':'DeliveryDwell',dropoff_arrived:!moving,dropoff_arrival_ms:arrival-10};h.status(s,t)}
 h.status({...idle(id),server_ms:arrival+6010},arrival+6020);
 }return h}
const fixed=run('firmware/RewardRocket393/WebUi342.h',3);
const expected=['intro','ten','nine','eight','seven','six','five','four','three','two','one','launch','blastoff'];
assert.deepEqual(fixed.events.map(e=>e.name),Array(3).fill(expected).flat(),'three consecutive complete spoken sequences, no refresh');
for(let i=0;i<3;i++){const ev=fixed.events.slice(i*13,(i+1)*13),arrival=24100+i*31000;assert(Math.abs(ev.find(e=>e.name==='launch').now-arrival)<100);for(let n=1;n<=10;n++){const e=ev.find(e=>e.name===expected[n]);assert(Math.abs(e.now-(arrival-(11-n)*1000))<100)}}
const h=harness('firmware/RewardRocket393/WebUi342.h');
assert.equal(h.api.rr342DropEtaMs({...idle(),run:'SeekHall',position_valid:false}),null,'manual/recovery unknown position still has no delivery estimate');
const active={...idle(1),mode:'Busy',credit:'Consumed',reward_in_progress:true,run:'SeekHall',position_valid:false,server_ms:100};
assert.equal(h.api.rr342DropEtaMs(active),24050,'untrusted-but-tracked reward position is only an audio estimate');
assert.equal(active.position_valid,false,'audio does not alter trusted position');
h.status(idle(),0);h.status(active,100);h.api.interruptAudio();h.status({...active,server_ms:200},200);assert.equal(h.events.length,1,'no replay after interruption/rebaseline');
console.log('PASS: three reward audio sequences, timing, manual-seek exclusion and interruption behavior');
