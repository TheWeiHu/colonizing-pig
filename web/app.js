'use strict';
const $ = id => document.getElementById(id);
const RES = ['Lumber', 'Brick', 'Wool', 'Grain', 'Ore'];
const palette = getComputedStyle(document.documentElement);
const COLORS = ['wood','brick','wool','grain','ore'].map(name => palette.getPropertyValue(`--${name}`).trim());
const DEV = ['Knight', 'Victory Point', 'Road Building', 'Year Of Plenty', 'Monopoly'];
const LAND = [[0,-2],[1,-2],[2,-2],[2,-1],[2,0],[1,1],[0,2],[-1,2],[-2,2],[-2,1],[-2,0],[-1,-1],[0,-1],[1,-1],[1,0],[0,1],[-1,1],[-1,0],[0,0]];
const PAIRS = RES.flatMap((_, a) => RES.slice(a).map((_, b) => [a, a+b]));
const SAVE_KEY = 'colonizing-pig-v1-porto-ruling';
let worker, generation = 0, request = 0, state = null, busy = true, ready = false, paused = false;
let mode = '', startTime = 0, timer = null, loadTimer = null, restoreOnReady = null, forcedSingle = false, log = [];
window.pigDiagnostics = {timings: [], workers: 0};

function readSave() {
  try {
    const save = JSON.parse(localStorage.getItem(SAVE_KEY));
    if (!save) return null;
    if (save.version !== 1 || !Number.isInteger(save.seed) || ![0,1].includes(save.human) || !Array.isArray(save.history) || save.history.length > 10000) throw Error();
    return save;
  } catch { $('load-status').textContent = 'The saved game could not be read. You can start a new one.'; return null; }
}
function persist() {
  if (!state) return;
  try {
    localStorage.setItem(SAVE_KEY, JSON.stringify({version:1, seed:state.seed, human:state.human, history:state.history, paused, log:log.slice(0,40)}));
    $('save-status').textContent = 'Saved On This Device';
  } catch { $('save-status').textContent = 'Storage Unavailable · Use Save File'; }
}
function send(type, data = {}) { worker.postMessage({id:++request, type, ...data}); }
function stopTimer() { clearInterval(timer); timer = null; $('game').classList.remove('thinking'); }
function fail(message) {
  busy = false; stopTimer();
  $('error').hidden = false; $('error-message').textContent = message;
  $('status').textContent = 'Game Paused';
  $('hint').textContent = 'Your last completed move is saved. Retry to continue.';
  $('start').disabled = true;
  $('actions').replaceChildren(); $('choice').replaceChildren();
  if (state) drawBoard();
}
function boot(saved = null) {
  worker?.terminate(); clearTimeout(loadTimer); const epoch = ++generation;
  ready = false; busy = true; stopTimer(); restoreOnReady = saved;
  $('start').disabled = true; $('start').textContent = 'Loading Model…';
  $('load-status').textContent = 'Loading the model on your device…';
  $('error').hidden = true;
  const threaded = !forcedSingle && self.crossOriginIsolated && typeof SharedArrayBuffer !== 'undefined';
  worker = new Worker('worker.js');
  loadTimer = setTimeout(() => {
    if (epoch !== generation || ready) return;
    if (threaded) { forcedSingle = true; boot(saved); }
    else { worker.terminate(); fail('Model loading timed out. Check your connection and retry.'); }
  }, 30000);
  worker.onerror = event => {
    if (epoch !== generation) return;
    if (!ready && threaded) { forcedSingle = true; boot(saved); return; }
    fail('The engine stopped unexpectedly. Retry to restore the last completed move.');
  };
  worker.onmessage = ({data}) => {
    if (epoch !== generation) return;
    if (data.type === 'ready') {
      clearTimeout(loadTimer);
      ready = true; window.pigDiagnostics.workers = data.workers;
      $('start').disabled = false; $('start').textContent = 'Play';
      $('load-status').textContent = 'Ready. Full-strength search runs on your device.';
      if (restoreOnReady) {
        log = Array.isArray(saved.log) ? saved.log.filter(x => typeof x === 'string').slice(0,40) : [];
        paused = Boolean(saved.paused);
        send('restore', saved);
      } else busy = false;
    } else if (data.type === 'state') {
      stopTimer(); busy = false; state = data.state;
      window.pigDiagnostics.lastState = state;
      if (data.elapsed !== undefined) {
        window.pigDiagnostics.timings.push({phase:state.phase, ms:data.elapsed});
        $('last-think').textContent = `Last Thought ${(data.elapsed/1000).toFixed(1)}s`;
      }
      if (!data.restored) appendEvents(state.events);
      $('welcome').hidden = true; $('game').hidden = false;
      mode = state.phase === 0 ? 'settlement' : [1,9].includes(state.phase) ? 'road' : state.phase === 5 ? 'robber' : '';
      persist(); render();
      if (!state.done && state.current !== state.human && !paused) think();
    } else if (data.type === 'error') {
      clearTimeout(loadTimer);
      if (!ready && threaded) { forcedSingle = true; boot(saved); return; }
      fail(data.message);
    }
  };
  send('init', {threaded, workers:Math.min(4, navigator.hardwareConcurrency || 1)});
}
function think() {
  busy = true; startTime = performance.now();
  $('game').classList.add('thinking'); render();
  timer = setInterval(() => {
    const elapsed = (performance.now()-startTime)/1000;
    $('status').textContent = `Pig Is Thinking · ${elapsed.toFixed(0)}s`;
    $('hint').textContent = elapsed > 15 ? 'Full search is still running. You can pause without losing your game.' : 'Same search. Your device sets the pace.';
  }, 500);
  send('bot');
}
function play(action) {
  if (!canPlay() || !state.legal.includes(action)) return;
  busy = true; render(); send('act', {action});
}
const canPlay = () => state && !state.done && !busy && !paused && $('error').hidden && state.current === state.human;
function button(label, click, className = '', enabled = true) {
  const b = document.createElement('button'); b.type = 'button'; b.textContent = label; b.className = className; b.disabled = !enabled; b.onclick = click; return b;
}
function chooseMode(next) { mode = mode === next ? '' : next; render(); }
function render() {
  if (!state) return;
  const me = state.players[state.human], rival = state.players[1-state.human];
  $('you-score').textContent = me.points; $('pig-score').textContent = rival.points;
  $('round').textContent = state.turn ? `Turn ${state.turn}` : 'Opening';
  $('dice').textContent = state.roll || '—';
  $('pause').textContent = paused ? 'Resume' : 'Pause'; $('pause').disabled = state.done;
  let title = 'Your Turn', hint = 'Build, trade, or end your turn.';
  if (state.done) { title = state.winner < 0 ? 'Game Drawn' : state.winner === state.human ? 'You Win!' : 'The Pig Wins'; hint = state.winner < 0 ? 'The move limit was reached.' : 'Fifteen points. Another island awaits.'; }
  else if (paused) { title = 'Game Paused'; hint = 'Resume whenever you are ready.'; }
  else if (busy && state.current !== state.human) { title = 'Pig Is Thinking'; hint = 'Same search. Your device sets the pace.'; }
  else if (busy) { title = 'Applying Your Move'; hint = 'One moment…'; }
  else if (state.phase === 0) { title = 'Place A Settlement'; hint = 'Choose a highlighted corner. Numbers with more dots produce more often.'; }
  else if ([1,9].includes(state.phase)) { title = 'Place A Road'; hint = 'Choose a highlighted edge.'; }
  else if (state.phase === 2) { title = 'Roll The Dice'; hint = 'You may play an eligible development card before rolling.'; }
  else if (state.phase === 4) { title = 'Discard Resources'; hint = 'Your hand is above nine. Choose one card at a time.'; }
  else if (state.phase === 5) { title = 'Move The Robber'; hint = 'Choose a highlighted tile. A steal happens automatically when allowed.'; }
  else if (mode === 'settlement') hint = 'Choose a highlighted corner for your new settlement.';
  else if (mode === 'city') hint = 'Choose one of your highlighted settlements to upgrade.';
  else if (mode === 'road') hint = 'Choose a highlighted edge for your road.';
  $('status').textContent = title; $('hint').textContent = hint;
  $('hand').replaceChildren(...RES.map((name,i) => {
    const div = document.createElement('div'); div.className = 'resource'; div.style.setProperty('--resource', COLORS[i]);
    const count = document.createElement('b'); count.textContent = me.hand[i];
    const label = document.createElement('span'); label.textContent = name; div.append(count,label); return div;
  }));
  $('dev-hand').textContent = me.dev.map((count,i) => count ? `${DEV[i]} ${count}${me.bought[i] ? ` (${me.bought[i]} New)` : ''}` : '').filter(Boolean).join(' · ') || 'No Development Cards';
  $('opponent').textContent = `Pig's Resources: ${RES.map((r,i)=>`${r} ${rival.hand[i]}`).join(' · ')} · Hidden Development Cards: ${rival.devCount}`;
  const owner = p => p < 0 ? 'Unclaimed' : p === state.human ? 'You' : 'Pig';
  $('awards').textContent = `Longest Road: ${owner(state.longestRoad)} (You ${me.roadLength} / Pig ${rival.roadLength}) · Largest Army: ${owner(state.largestArmy)} (You ${me.played[0]} / Pig ${rival.played[0]})`;
  $('bank').textContent = `Bank: ${RES.map((r,i)=>`${r} ${state.bank[i]}`).join(' · ')}`;
  $('log').replaceChildren(...log.slice(0,40).map(text=>{const li=document.createElement('li');li.textContent=text;return li;}));
  renderActions(); drawBoard();
}
function renderActions() {
  $('actions').replaceChildren(); $('choice').replaceChildren();
  if (!canPlay()) return;
  const legal = state.legal;
  const has = (a,b) => legal.some(x=>x>=a&&x<=b);
  const add = (label, action, primary=false) => { if(legal.includes(action)) $('actions').append(button(label,()=>play(action),primary?'primary':'')); };
  const toggle = (label,key) => { const b=button(label,()=>chooseMode(key));b.setAttribute('aria-pressed', mode===key);$('actions').append(b); };
  add('Roll Dice',180,true); add('End Turn',181,true);
  if (state.phase === 7) {
    if(has(0,53)) toggle('Settlement','settlement');
    if(has(54,125)) toggle('Road','road');
    if(has(126,179)) toggle('City','city');
  }
  add('Buy Card',182); add('Play Knight',183); add('Road Building',184);
  if(has(185,199)) toggle('Year Of Plenty','plenty');
  if(has(200,204)) toggle('Monopoly','monopoly');
  if(has(229,248)) toggle('Bank Trade','trade');
  for(let i=0;i<5;i++) add(`Discard ${RES[i]}`,224+i);
  if (mode==='trade' && has(229,248)) {
    const options=legal.filter(x=>x>=229).map(action=>{
      const n=action-229,give=Math.floor(n/4),receive=n%4+(n%4>=give?1:0);
      return [action,`${state.players[state.human].ratio[give]} ${RES[give]} → 1 ${RES[receive]}`];
    }); selectAction('Trade',options);
  } else if(mode==='plenty' && has(185,199)) selectAction('Take Resources',legal.filter(x=>x>=185&&x<=199).map(action=>[action,PAIRS[action-185].map(i=>RES[i]).join(' + ')]));
  else if(mode==='monopoly' && has(200,204)) selectAction('Take All',legal.filter(x=>x>=200&&x<=204).map(action=>[action,RES[action-200]]));
}
function selectAction(label,options) {
  const form=document.createElement('form'), select=document.createElement('select');select.setAttribute('aria-label',label);
  options.forEach(([id,text])=>select.add(new Option(text,id)));
  const submit=button(label,()=>play(Number(select.value)),'primary');
  form.onsubmit=e=>{e.preventDefault();play(Number(select.value));};form.append(select,submit);$('choice').append(form);
}
function svg(tag, attrs={}, children=[]) {
  const el=document.createElementNS('http://www.w3.org/2000/svg',tag);
  for(const [key,value] of Object.entries(attrs))el.setAttribute(key,value);
  for(const child of children)el.append(typeof child==='string'?document.createTextNode(child):child);
  return el;
}
function interactive(label,action,children) {
  const group=svg('g',{class:'target',role:'button',tabindex:0,'aria-label':label},children);
  group.onclick=()=>play(action);group.onkeydown=e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();play(action);}};
  return group;
}
function drawBoard() {
  if(!state)return;
  const board=$('board');board.replaceChildren();
  const size=47, nodes=Array(54), centers=LAND.map(([q,r])=>[Math.sqrt(3)*size*(q+r/2),1.5*size*r]);
  const points=(x,y)=>Array.from({length:6},(_,k)=>[x+size*Math.cos((-90+k*60)*Math.PI/180),y+size*Math.sin((-90+k*60)*Math.PI/180)]);
  const fmt=p=>p.map(v=>v.toFixed(2)).join(',');
  const available=action=>canPlay()&&state.legal.includes(action);
  centers.forEach(([x,y],i)=>{
    const corners=points(x,y);corners.forEach((point,k)=>nodes[state.tileNodes[i][k]]=point);
    const resource=state.tiles[i], number=state.numbers[i];
    const tile=svg('polygon',{points:corners.map(fmt).join(' '),class:'tile',fill:resource<0?palette.getPropertyValue('--sand').trim():COLORS[resource]});
    const name=resource<0?'Desert':RES[resource];
    const group=svg('g',{},[tile,svg('title',{},[`${name}, ${number||'no production'}`]),svg('text',{x,y:y-20,class:'tile-label',style:resource===0?'fill:#fff':''},[name])]);
    if(number){
      group.append(svg('circle',{cx:x,cy:y+6,r:19,class:'number-token'}),svg('text',{x,y:y+10,class:'number-text'},[String(number)]));
      const count=6-Math.abs(7-number);
      for(let k=0;k<count;k++)group.append(svg('circle',{cx:x+(k-(count-1)/2)*4,cy:y+18,r:1.4,class:'pips'}));
    }
    if(state.robber===i)group.append(svg('g',{'aria-label':'Robber'},[svg('path',{d:`M ${x+23} ${y+7} l -5 15 h 14 l -5 -15`,fill:'#152835',stroke:'#f4f1e9','stroke-width':1.2}),svg('circle',{cx:x+25,cy:y+4,r:5,fill:'#152835',stroke:'#f4f1e9','stroke-width':1.2})]));
    if(mode==='robber'&&available(205+i)) group.append(interactive(`Move robber to ${name} ${number||'desert'}`,205+i,[svg('polygon',{points:corners.map(fmt).join(' '),class:'target-marker',fill:'transparent'})]));
    board.append(group);
  });
  state.portNodes.forEach(([a,b],i)=>{
    const x=(nodes[a][0]+nodes[b][0])/2,y=(nodes[a][1]+nodes[b][1])/2,d=Math.hypot(x,y),px=x+x/d*34,py=y+y/d*34;
    board.append(svg('line',{x1:x,y1:y,x2:px,y2:py,class:'port-line'}),svg('text',{x:px,y:py+4,class:'port-label'},[state.ports[i]<0?'3:1':`${RES[state.ports[i]]} 2:1`]));
  });
  state.edges.forEach(([a,b],edge)=>{
    const [x1,y1]=nodes[a],[x2,y2]=nodes[b];
    board.append(svg('line',{x1,y1,x2,y2,class:'road-base'}));
    for(let p=0;p<2;p++) if(state.players[p].roads[edge])board.append(svg('line',{x1,y1,x2,y2,stroke:p===state.human?'#78d7ff':'#d4aeff','stroke-width':7,'stroke-linecap':'round','stroke-dasharray':p===state.human?'none':'9 3'}));
    if(mode==='road'&&available(54+edge))board.append(interactive(`Build road ${edge+1}`,54+edge,[svg('line',{x1,y1,x2,y2,class:'target-marker','stroke-width':6}),svg('line',{x1,y1,x2,y2,stroke:'transparent','stroke-width':22}),svg('circle',{cx:(x1+x2)/2,cy:(y1+y2)/2,r:15,class:'hit'})]));
  });
  nodes.forEach(([x,y],node)=>{
    for(let p=0;p<2;p++) {
      const level=state.players[p].nodes[node]; if(!level)continue;
      const color=p===state.human?'#78d7ff':'#d4aeff';
      const shape=p===state.human?svg('circle',{cx:x,cy:y,r:10,fill:color,stroke:'#102c40','stroke-width':2}):svg('path',{d:`M ${x} ${y-13} L ${x+13} ${y} L ${x} ${y+13} L ${x-13} ${y} Z`,fill:color,stroke:'#102c40','stroke-width':2});
      board.append(shape,svg('text',{x,y:y+4,'text-anchor':'middle','font-size':11,'font-weight':800,fill:'#102c40','pointer-events':'none'},[level===2?'Ⅱ':'Ⅰ']));
    }
    const action=mode==='city'?126+node:node;
    if(['city','settlement'].includes(mode)&&available(action))board.append(interactive(`${mode==='city'?'Upgrade city':'Build settlement'} ${node+1}`,action,[svg('circle',{cx:x,cy:y,r:12,class:'target-marker'}),svg('circle',{cx:x,cy:y,r:17,class:'hit'})]));
  });
}
function actionName(a) {
  if(a<54)return 'Placed A Settlement';if(a<126)return 'Built A Road';if(a<180)return 'Upgraded A City';
  if(a===180)return 'Rolled';if(a===181)return 'Ended The Turn';if(a===182)return 'Bought A Development Card';if(a===183)return 'Played A Knight';if(a===184)return 'Played Road Building';
  if(a<200)return `Took ${PAIRS[a-185].map(i=>RES[i]).join(' + ')}`;
  if(a<205)return `Played Monopoly For ${RES[a-200]}`;if(a<224)return 'Moved The Robber';if(a<229)return `Discarded ${RES[a-224]}`;
  const n=a-229,g=Math.floor(n/4),r=n%4+(n%4>=g?1:0);return `Traded ${RES[g]} For ${RES[r]}`;
}
function appendEvents(events) {
  for(const [player,action,phase,outcome] of events) {
    const who=player===state.human?'You':'Pig';
    if(action>=0&&action!==180)log.unshift(`${who}: ${actionName(action)}`);
    else if(phase===3)log.unshift(`${who}: Rolled ${outcome+2}`);
    else if(phase===6&&outcome>=0)log.unshift(`${who}: Stole ${RES[outcome]}`);
    else if(phase===8&&player===state.human&&outcome>=0)log.unshift(`You Drew ${DEV[outcome]}`);
  }
  log=log.slice(0,40);
}
$('start-form').onsubmit=e=>{
  e.preventDefault();if(!ready)return;
  const [seed, draw]=crypto.getRandomValues(new Uint32Array(2));
  log=[];paused=false;busy=true;send('new',{seed,human:draw & 1});
};
$('pause').onclick=()=>{
  if(!state||state.done)return;
  paused=!paused;persist();
  if(paused){worker.terminate();generation++;ready=false;busy=false;stopTimer();render();}
  else boot({...readSave(),paused:false});
};
$('zoom').onclick=()=>{
  const zoomed=$('board-wrap').classList.toggle('zoomed');$('zoom').setAttribute('aria-pressed',zoomed);$('zoom').textContent=zoomed?'Fit Board':'Zoom Board';
  if(zoomed){$('board-wrap').scrollLeft=($('board-wrap').scrollWidth-$('board-wrap').clientWidth)/2;$('board-wrap').scrollTop=200;}
};
$('new-game').onclick=()=>$('new-dialog').showModal();
$('new-dialog').onclose=()=>{
  if($('new-dialog').returnValue!=='new')return;
  state=null;log=[];paused=false;try{localStorage.removeItem(SAVE_KEY);}catch{}
  $('game').hidden=true;$('welcome').hidden=false;boot();
};
$('export').onclick=()=>{
  if(!state)return;
  const data={version:1,seed:state.seed,human:state.human,history:state.history,paused:true,log};
  const url=URL.createObjectURL(new Blob([JSON.stringify(data)],{type:'application/json'}));
  const a=document.createElement('a');a.href=url;a.download=`colonizing-pig-${state.seed}.json`;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
};
document.querySelectorAll('.load-file').forEach(b=>b.onclick=()=>$('load-file').click());
$('load-file').onchange=async()=>{
  const file=$('load-file').files[0];$('load-file').value='';if(!file)return;
  try{
    if(file.size>250000)throw Error('Saved game is too large.');
    const save=JSON.parse(await file.text());
    if(save.version!==1||!Number.isInteger(save.seed)||save.seed<0||save.seed>0xffffffff||![0,1].includes(save.human)||!Array.isArray(save.history)||save.history.length>10000||save.history.some(a=>!Number.isInteger(a)||a<0||a>=249))throw Error('This is not a valid Colonizing Pig save.');
    // Keep the last good save on disk until the worker validates every move.
    paused=true;boot({...save,paused:true});
  }catch(error){fail(error.message);}
};
$('retry').onclick=()=>{forcedSingle=true;boot(readSave());};
$('discard-save').onclick=()=>{try{localStorage.removeItem(SAVE_KEY);}catch{}state=null;log=[];paused=false;$('game').hidden=true;$('welcome').hidden=false;boot();};
boot(readSave());
