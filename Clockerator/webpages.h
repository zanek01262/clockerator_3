/* =====================================================================
   webpages.h  —  PROGMEM HTML for the setup splash and live dashboard
   ---------------------------------------------------------------------
   Theme: Brass & Walnut (walnut panels, aged-brass accents). Canvas
   colours come from CSS variables, so retheming is a :root edit only.

   Two pages:
     SETUP_PAGE  — served in AP mode. Scans networks (/scan), lets the
                   user pick one + enter a password, POSTs to /save.
                   No external resources: setup clients have no internet.
     DASH_PAGE   — served in STA mode. Polls /api/state, /api/mic,
                   /api/events, /api/rec and /api/sd; live plot, chime
                   events with recordings (play in the page), detection
                   tuning, microphone health, live spectrum +
                   spectrogram, SD card status, speed test + file
                   browser, chime log viewer and Excel export.
   Levels on the page are sound pressure: pascals internally, shown as
   dB SPL (20 µPa reference).
   ===================================================================== */
#ifndef WEBPAGES_H
#define WEBPAGES_H

#include <Arduino.h>

// ----------------------------------------------------------------------
//  SETUP / SPLASH PAGE  (AP mode)
// ----------------------------------------------------------------------
static const char SETUP_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clockerator &mdash; Setup</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Ccircle cx='16' cy='16' r='13.5' fill='%23231b15' stroke='%23c9a227' stroke-width='3'/%3E%3Cpath d='M16 8.5V16l5 3.5' stroke='%23f2e8d9' stroke-width='2.6' fill='none' stroke-linecap='round' stroke-linejoin='round'/%3E%3C/svg%3E">
<meta name="theme-color" content="#191310">
<style>
  /* Brass & Walnut, self-contained. NO external fonts: AP-mode clients
     have no internet and a render-blocking font fetch hangs the page. */
  :root{
    --bg:#191310; --panel:#231b15; --panel2:#2d231a; --line:#3f3125;
    --ink:#f2e8d9; --dim:#b9a691; --faint:#8b7860;
    --accent:#c9a227; --accent-dim:#7d6418; --accent-soft:rgba(201,162,39,.13);
    --good:#8ba645; --bad:#c1543c;
    color-scheme:dark;
    --display:Georgia,'Times New Roman',serif;
    --body:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;
  }
  *{box-sizing:border-box}
  html,body{margin:0;min-height:100%}
  body{background:var(--bg); color:var(--ink); min-height:100vh;
    font-family:var(--body); display:flex; align-items:center;
    justify-content:center; padding:20px; -webkit-font-smoothing:antialiased}
  .panel{width:100%; max-width:420px; background:var(--panel);
    background-image:linear-gradient(180deg,rgba(255,255,255,.025),transparent 60%);
    border:1px solid var(--line); border-radius:12px; padding:28px 26px 30px;
    box-shadow:inset 0 1px 0 rgba(255,255,255,.04)}
  .brand{display:flex; align-items:center; gap:10px; margin-bottom:4px}
  .brand::before{content:""; width:9px; height:9px; border-radius:50%;
    background:var(--accent); box-shadow:0 0 0 4px var(--accent-soft)}
  h1{font-family:var(--display); font-size:23px; font-weight:700; margin:0;
    letter-spacing:.01em}
  .sub{color:var(--faint); font-size:13px; margin:4px 0 22px}
  label{display:block; font-family:var(--display); font-size:13px; font-weight:700;
    letter-spacing:.09em; text-transform:uppercase; color:var(--accent); margin:18px 0 8px}
  .net{border:1px solid var(--line); border-radius:9px; max-height:210px;
    overflow:auto; background:var(--panel2)}
  .net button{display:flex; width:100%; align-items:center; gap:10px;
    background:none; border:0; border-bottom:1px solid var(--line);
    color:var(--ink); font-family:inherit; font-size:14.5px; padding:13px 14px;
    cursor:pointer; text-align:left; transition:background .12s}
  .net button:last-child{border-bottom:0}
  .net button:hover,.net button.sel{background:var(--accent-soft); color:var(--accent)}
  .net .ss{flex:1; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; font-weight:500}
  .net .meta{font-size:12px; color:var(--faint)}
  input[type=password],input[type=text]{width:100%; background:var(--panel2);
    border:1px solid var(--line); border-radius:9px; color:var(--ink);
    font-family:inherit; font-size:15px; padding:13px 14px; outline:none;
    transition:border-color .15s}
  input:focus{border-color:var(--accent)}
  .row{display:flex; gap:10px; align-items:center; margin-top:10px}
  .ghost{color:var(--dim); font-size:13px; background:none; border:0; cursor:pointer;
    font-family:var(--body); font-weight:500; padding:0}
  .ghost:hover{color:var(--accent)}
  label .ghost{display:inline; margin-left:8px; text-transform:none;
    letter-spacing:0; font-size:12px; font-family:var(--body); font-weight:500}
  button.go{margin-top:24px; width:100%; padding:14px; border-radius:9px; border:0;
    background:var(--accent); color:#2a2008; font-family:var(--display);
    font-weight:700; font-size:16px; letter-spacing:.06em; text-transform:uppercase;
    cursor:pointer; transition:background .15s}
  button.go:hover{background:#dcb433}
  button.go:disabled{opacity:.4; cursor:not-allowed}
  .status{margin-top:16px; font-size:13.5px; min-height:18px; line-height:1.5}
  .status.ok{color:var(--good)} .status.err{color:#e08a7a}
</style></head>
<body>
  <div class="panel">
    <div class="brand"><h1>Clockerator</h1></div>
    <div class="sub">Connect your device to Wi-Fi</div>

    <label>Network <button class="ghost" id="rescan" type="button">Rescan</button></label>
    <div class="net" id="nets"><button type="button" disabled>Scanning&hellip;</button></div>

    <label>Password</label>
    <input type="password" id="pass" placeholder="Wi-Fi password" autocomplete="off">
    <div class="row"><button class="ghost" id="showpw" type="button">Show password</button></div>

    <button class="go" id="save" type="button" disabled>Connect</button>
    <div class="status" id="st"></div>
  </div>
<script>
  let chosen = null;
  const netsEl = document.getElementById('nets');
  const saveBtn = document.getElementById('save');
  const st = document.getElementById('st');

  function bars(rssi){
    if(rssi>=-55) return '▂▄▆█';
    if(rssi>=-65) return '▂▄▆_';
    if(rssi>=-75) return '▂▄__';
    return '▂___';
  }
  function render(list){
    netsEl.innerHTML='';
    if(!list.length){ netsEl.innerHTML='<button type="button" disabled>no networks found</button>'; return; }
    list.sort((a,b)=>b.rssi-a.rssi);
    list.forEach(n=>{
      const b=document.createElement('button'); b.type='button';
      b.innerHTML='<span class="ss">'+n.ssid+'</span>'+
                  '<span class="meta">'+(n.lock?'🔒 ':'')+bars(n.rssi)+'</span>';
      b.onclick=()=>{ chosen=n.ssid;
        [...netsEl.children].forEach(c=>c.classList.remove('sel'));
        b.classList.add('sel'); saveBtn.disabled=false; };
      netsEl.appendChild(b);
    });
  }
  function scan(){
    netsEl.innerHTML='<button type="button" disabled>scanning…</button>';
    fetch('/scan').then(r=>r.json()).then(render)
      .catch(()=>netsEl.innerHTML='<button type="button" disabled>scan failed</button>');
  }
  document.getElementById('rescan').onclick=function(){
    netsEl.innerHTML='<button type="button" disabled>scanning\u2026</button>';
    fetch('/rescan').catch(()=>{});
    setTimeout(scan,3000);   // async scan finishes well within 3s
  };
  document.getElementById('showpw').onclick=function(){
    const p=document.getElementById('pass');
    const show=p.type==='password'; p.type=show?'text':'password';
    this.textContent=show?'hide password':'show password';
  };
  saveBtn.onclick=function(){
    if(!chosen) return;
    saveBtn.disabled=true; st.className='status'; st.textContent='saving…';
    const body='ssid='+encodeURIComponent(chosen)+
               '&pass='+encodeURIComponent(document.getElementById('pass').value);
    fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body})
      .then(r=>r.json()).then(j=>{
        if(j.ok){ st.className='status ok';
          st.textContent='Saved. Device is rebooting and joining '+chosen+'. You can close this page.'; }
        else { st.className='status err'; st.textContent='Save failed: '+(j.err||'unknown'); saveBtn.disabled=false; }
      }).catch(()=>{ st.className='status ok';
        st.textContent='Device rebooting… (connection dropped, which is expected)'; });
  };
  scan();
</script>
</body></html>
)HTML";

// ----------------------------------------------------------------------
//  LIVE DASHBOARD  (STA mode)
// ----------------------------------------------------------------------
static const char DASH_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clockerator</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Ccircle cx='16' cy='16' r='13.5' fill='%23231b15' stroke='%23c9a227' stroke-width='3'/%3E%3Cpath d='M16 8.5V16l5 3.5' stroke='%23f2e8d9' stroke-width='2.6' fill='none' stroke-linecap='round' stroke-linejoin='round'/%3E%3C/svg%3E">
<meta name="theme-color" content="#191310">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Cormorant+Garamond:wght@600;700&family=Inter:wght@400;500;600&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
<style>
  /* ---- Brass & Walnut theme ----
     Palette drawn from a longcase clock's case: walnut body, aged brass
     furniture, warm ivory dial text. Canvas colours are exposed as vars
     (--grid/--trace/--thresh/--axis) so the graphs match the chrome. */
  :root{
    --bg:#191310; --panel:#231b15; --panel2:#2d231a; --line:#3f3125;
    --ink:#f2e8d9; --dim:#b9a691; --faint:#8b7860;
    --accent:#c9a227; --accent-dim:#7d6418; --accent-soft:rgba(201,162,39,.13);
    --good:#8ba645; --warn:#d69128; --bad:#c1543c;
    --radius:10px;
    color-scheme:dark;
    /* canvas */
    --grid:rgba(201,162,39,.11); --thresh:rgba(214,145,40,.65);
    --trace:#e0c158; --traceglow:rgba(224,193,88,.55); --axis:rgba(201,162,39,.5);
    /* spectrogram heat ramp: quiet -> loud */
    --heat0:#120c08; --heat1:#5a3414; --heat2:#c9a227; --heat3:#f2e8d9;
    /* type */
    --display:'Cormorant Garamond',Georgia,'Times New Roman',serif;
    --body:'Inter',system-ui,-apple-system,sans-serif;
    --mono:'JetBrains Mono',ui-monospace,monospace;
  }
  *{box-sizing:border-box}
  html,body{margin:0}
  body{background:var(--bg); color:var(--ink); font-family:var(--body);
    font-size:15px; line-height:1.5; -webkit-font-smoothing:antialiased}
  .wrap{max-width:1120px; margin:0 auto; padding:24px 20px 64px}
  .topbar{height:3px; background:linear-gradient(90deg,var(--accent),rgba(201,162,39,.15) 55%,transparent)}

  /* header */
  header{display:flex; align-items:center; justify-content:space-between;
    gap:16px; padding:8px 0 22px; flex-wrap:wrap}
  h1{font-family:var(--display); font-weight:700; font-size:27px; letter-spacing:.01em;
    margin:0; color:var(--ink); display:flex; align-items:center; gap:10px}
  h1::before{content:""; width:9px; height:9px; border-radius:50%;
    background:var(--accent); box-shadow:0 0 0 4px rgba(201,162,39,.16)}
  .hdr-r{display:flex; align-items:center; gap:14px; flex-wrap:wrap}
  .clock{font-family:var(--mono); font-size:15px; color:var(--dim);
    font-variant-numeric:tabular-nums; letter-spacing:.02em}
  .rec{font-size:12px; font-weight:600; letter-spacing:.05em; text-transform:uppercase;
    padding:4px 10px; border-radius:20px; border:1px solid var(--line); color:var(--faint)}
  .rec::before{content:"\25CF\00a0"}
  .rec.on{color:var(--good); border-color:rgba(139,166,69,.45)}
  .rec.off{color:#e08a7a; border-color:rgba(193,84,60,.5)}

  /* layout: two-column on wide screens */
  .cols{display:grid; grid-template-columns:1fr; gap:18px}
  @media(min-width:880px){ .cols{grid-template-columns:1.35fr 1fr} }
  .col{display:flex; flex-direction:column; gap:18px; min-width:0}

  /* panels */
  .panel{background:var(--panel); border:1px solid var(--line); border-radius:var(--radius);
    padding:20px; background-image:linear-gradient(180deg,rgba(255,255,255,.025),transparent 60%);
    box-shadow:inset 0 1px 0 rgba(255,255,255,.04)}
  .panel h2{font-family:var(--display); font-weight:700; font-size:15px;
    letter-spacing:.10em; text-transform:uppercase; color:var(--accent); margin:0 0 16px}

  /* scope */
  .scope{border:1px solid var(--line); border-radius:10px; overflow:hidden;
    background:#120c08; position:relative}
  canvas{display:block; width:100%; height:200px}   /* = the height drawScope() draws */
  .scopectl{display:flex; align-items:center; gap:10px; margin-top:14px; flex-wrap:wrap}
  .scopectl label{font-size:12px; color:var(--faint); font-weight:500}
  .scopectl .pair{display:inline-flex; align-items:center; gap:8px}   /* label wraps with its control */
  .scopectl .win{margin-left:auto; font-size:12px; color:var(--faint); font-family:var(--mono)}

  /* stat tiles */
  .grid{display:grid; grid-template-columns:repeat(auto-fit,minmax(130px,1fr)); gap:12px; margin:0}
  /* four tiles: 2 x 2, or one row once the column is wide enough for it */
  .grid.four{grid-template-columns:repeat(2,1fr)}
  @media(min-width:1080px){ .grid.four{grid-template-columns:repeat(4,1fr)} }
  .stat{background:var(--panel2); border:1px solid var(--line); border-radius:11px; padding:14px 16px}
  .stat .k{font-size:11px; letter-spacing:.05em; text-transform:uppercase;
    color:var(--faint); font-weight:600; margin-bottom:6px}
  .stat .v{font-family:var(--display); font-size:29px; font-weight:700; color:var(--ink);
    font-variant-numeric:tabular-nums; letter-spacing:-.02em}
  .stat .s{font-family:var(--mono); font-size:11.5px; color:var(--faint); margin-top:3px;
    white-space:nowrap; overflow:hidden; text-overflow:ellipsis}

  .hk{font-size:13px; color:var(--dim); font-weight:500}

  /* controls */
  .ctl{display:flex; align-items:center; gap:14px; margin:12px 0}
  .ctl > label{font-size:13px; color:var(--dim); width:92px; flex:none; font-weight:500}
  input[type=range]{flex:1; accent-color:var(--accent); height:4px}
  .ctl output{font-family:var(--mono); color:var(--accent); width:78px; text-align:right;
    font-size:14px; font-variant-numeric:tabular-nums}
  .sub{font-size:12px; color:var(--faint); margin:-6px 0 6px 106px; font-family:var(--mono)}

  .actions{display:flex; gap:10px; flex-wrap:wrap; align-items:center}
  button.btn, a.btn{font-family:var(--body); font-weight:600; font-size:13.5px;
    text-decoration:none; color:var(--ink); border:1px solid var(--line);
    background:var(--panel2); padding:9px 15px; border-radius:9px; cursor:pointer;
    transition:all .15s; letter-spacing:-.005em; white-space:nowrap}
  button.btn:hover, a.btn:hover{border-color:var(--accent-dim); background:var(--accent-soft); color:var(--accent)}
  button.btn:active{transform:translateY(1px)}
  button.btn:disabled{opacity:.5; cursor:default; pointer-events:none}
  /* keyboard focus: visible ring (mouse clicks don't show it) */
  button:focus-visible,a:focus-visible,select:focus-visible,input:focus-visible{
    outline:2px solid var(--accent); outline-offset:2px}
  button.primary{background:var(--accent); border-color:var(--accent); color:#2a2008}
  button.primary:hover{background:#dcb433; border-color:#dcb433; color:#2a2008}
  button.danger{color:#f0a8a3; border-color:rgba(193,84,60,.4)}
  button.danger:hover{background:rgba(193,84,60,.12); border-color:var(--bad); color:#ffb4ae}

  select{background:var(--panel2); border:1px solid var(--line); border-radius:9px;
    color:var(--ink); font-family:var(--body); font-size:13.5px; padding:8px 11px;
    outline:none; cursor:pointer; transition:border-color .15s}
  select:focus{border-color:var(--accent)}
  .toggle-label{font-size:11px; letter-spacing:.05em; text-transform:uppercase;
    color:var(--faint); font-weight:600; margin-left:auto}
  .saved{font-size:13px; color:var(--good); opacity:0; transition:opacity .3s; font-weight:600}
  .saved.show{opacity:1}

  /* help mode */
  #helpToggle.on{background:var(--accent);border-color:var(--accent);color:#2a2008}
  body.help [data-help]{cursor:help; position:relative;
    outline:1px dashed var(--accent-dim); outline-offset:3px; border-radius:4px}
  .tip{position:fixed; z-index:50; max-width:290px; background:#120c08;
    border:1px solid var(--accent-dim); border-radius:10px; padding:11px 13px;
    font-size:13px; line-height:1.45; color:var(--ink); pointer-events:none;
    box-shadow:0 8px 30px rgba(0,0,0,.5); opacity:0; transition:opacity .12s}
  .tip.show{opacity:1}
  .tip .tt{display:block; font-weight:700; color:var(--accent); margin-bottom:3px;
    font-family:var(--display); font-size:14px}

  /* level meter */
  .gbar{position:relative; height:30px; background:var(--panel2);
    border:1px solid var(--line); border-radius:9px; overflow:hidden}
  .gfill{position:absolute; top:0; bottom:0; left:0;
    background:linear-gradient(90deg,var(--accent-dim),var(--accent)); transition:width .15s; opacity:.85}
  .mk{position:absolute; top:0; bottom:0; width:2px; transition:left .3s}
  .mk.amb{background:var(--dim)}
  .mk.trg{background:var(--warn); box-shadow:0 0 0 1px rgba(0,0,0,.35)}
  .mscale{display:flex; justify-content:space-between; font-family:var(--mono);
    font-size:10.5px; color:var(--faint); margin-top:4px}
  .gverdict{font-family:var(--display); font-weight:700; font-size:19px; margin:12px 0 4px}
  .gverdict.good{color:var(--good)} .gverdict.warn{color:var(--warn)}
  .gverdict.bad{color:#e08a7a} .gverdict.dim{color:var(--dim)}
  .gadvice{font-size:13.5px; color:var(--dim); line-height:1.5; min-height:38px}
  .gnums{display:flex; gap:16px; flex-wrap:wrap; margin-top:10px; font-family:var(--mono);
    font-size:12.5px; color:var(--faint)}
  .gnums b{color:var(--ink); font-weight:500}
  .gnums.sep{margin-top:8px; padding-top:8px; border-top:1px solid var(--line)}

  /* SD card */
  .sdstate{display:flex; align-items:center; gap:10px; font-weight:600; font-size:14.5px}
  .sdstate .dot{width:10px; height:10px; border-radius:50%; background:var(--faint); flex:none}
  .sdstate.on .dot{background:var(--good); box-shadow:0 0 0 4px rgba(139,166,69,.15)}
  .sdstate.off .dot{background:var(--bad); box-shadow:0 0 0 4px rgba(193,84,60,.15)}
  .sdstate.off{color:#e08a7a}
  .sdstate .sdtxt{flex:1; min-width:0}
  .sdstate .nx{color:var(--faint); font-weight:500; font-size:13px; white-space:nowrap}
  .sdstate .btn{padding:5px 12px; font-size:12.5px; flex:none}
  .files{margin-top:14px; max-height:300px; overflow:auto; border:1px solid var(--line); border-radius:9px}
  .files .fh{padding:8px 12px; font-size:11px; letter-spacing:.05em; text-transform:uppercase;
    color:var(--dim); font-weight:600; background:var(--panel2); border-bottom:1px solid var(--line)}
  .files .fr{display:flex; align-items:center; gap:10px; padding:7px 12px;
    border-bottom:1px solid rgba(63,49,37,.6); font-family:var(--mono); font-size:12.5px}
  .files .fr:last-child{border-bottom:0}
  .files .fr a{color:var(--accent); text-decoration:none}
  .files .fr a:hover{text-decoration:underline}
  .files .fr .sz{margin-left:auto; color:var(--faint)}
  .files button.mo{background:none; border:0; color:var(--accent); cursor:pointer;
    font-family:var(--mono); font-size:12.5px; padding:0}
  .files .sub2{padding-left:22px}

  /* log viewer table */
  #logTable th{position:sticky;top:0;background:var(--panel2);color:var(--dim);
    font-family:var(--body);font-weight:600;font-size:11px;letter-spacing:.05em;
    text-transform:uppercase;text-align:left;padding:8px 10px;border-bottom:1px solid var(--line)}
  #logTable td{padding:7px 10px;border-bottom:1px solid rgba(63,49,37,.6);
    font-family:var(--mono);font-size:12px;color:var(--ink);white-space:nowrap}
  #logTable tr:last-child td{border-bottom:0}

  h1 .ver{font-family:var(--mono); font-size:12px; font-weight:500; color:var(--accent);
    border:1px solid var(--accent-dim); border-radius:20px; padding:2px 9px; letter-spacing:.04em}
  .rec.live{color:#f0a8a3; border-color:rgba(193,84,60,.6)}
  .rec.live::before{animation:pulse 1s infinite}

  /* chime events */
  .recline{display:flex; align-items:center; gap:9px; font-size:13.5px; color:var(--dim); margin-bottom:12px}
  .recdot{width:10px; height:10px; border-radius:50%; background:var(--faint); flex:none}
  .recdot.on{background:var(--bad); box-shadow:0 0 0 4px rgba(193,84,60,.18); animation:pulse 1s infinite}
  @keyframes pulse{50%{opacity:.35}}
  audio{display:block; width:100%; height:38px; margin-bottom:12px}
  audio[hidden],.nowplay[hidden]{display:none}
  .nowplay{font-family:var(--mono); font-size:12px; color:var(--dim); margin:-4px 0 8px}
  .evlist{max-height:340px; overflow:auto; border:1px solid var(--line); border-radius:9px}
  .ev{display:grid; grid-template-columns:76px 72px 1fr auto; align-items:center; gap:8px;
    padding:9px 12px; border-bottom:1px solid rgba(63,49,37,.6)}
  .ev:last-child{border-bottom:0}
  .ev.now{background:var(--accent-soft)}
  .ev.playing{background:rgba(139,166,69,.12)}
  .ev .evt{font-family:var(--mono); font-size:13px; line-height:1.25}
  .ev .evt small{display:block; color:var(--faint); font-size:11px}
  .ev .evn{font-family:var(--display); font-size:24px; font-weight:700; line-height:1; white-space:nowrap}
  .ev .evn small{font-family:var(--body); font-size:10px; color:var(--faint); font-weight:600;
    letter-spacing:.05em; text-transform:uppercase; margin-left:4px}
  .ev .evm{font-family:var(--mono); font-size:12px; color:var(--dim); min-width:0}
  @media(max-width:520px){         /* phone: the figures get a line of their own */
    .ev{grid-template-columns:76px 1fr auto}
    .ev .eva{grid-column:3; grid-row:1}
    .ev .evm{grid-column:1/-1; grid-row:2}
  }
  .ctl output{white-space:nowrap}
  .ev .eva{display:flex; gap:6px}
  .ev .eva button,.ev .eva a,.files button.pl{background:var(--panel2); border:1px solid var(--line);
    color:var(--accent); border-radius:7px; width:32px; height:30px; display:inline-flex;
    align-items:center; justify-content:center; cursor:pointer; text-decoration:none; font-size:13px; padding:0}
  .ev .eva button:hover,.ev .eva a:hover,.files button.pl:hover{border-color:var(--accent-dim); background:var(--accent-soft)}
  .ev .eva .na{color:var(--faint); font-size:11.5px; font-family:var(--mono)}
  .files button.pl{width:26px; height:22px; font-size:11px}
  .evempty{padding:14px 12px; color:var(--faint); font-size:13px}
  .opts{display:flex; flex-wrap:wrap; gap:10px 16px; align-items:center;
    margin-top:14px; padding-top:14px; border-top:1px solid var(--line)}
  .opts label{font-size:12px; color:var(--faint); font-weight:500; display:flex; align-items:center; gap:8px}

  /* spectrum */
  #spec,#wf{height:150px}
  .specgap{margin-top:8px}
</style></head>
<body>
<div class="topbar"></div>
<div class="wrap">
  <header>
    <h1>Clockerator <span class="ver">3.0</span></h1>
    <div class="hdr-r">
      <span class="rec" id="rec" data-help="tt:Logging|Green when chimes and levels are being saved to the SD card; red-pulsing while a chime is being recorded as sound. Red means nothing is being saved: no card, a card error, or no network time yet.">SD</span>
      <button class="btn" id="helpToggle" title="Explain controls">? Help</button>
      <div class="clock" id="clock">--:--:--</div>
    </div>
  </header>

  <div class="cols">
    <div class="col">

      <div class="panel">
        <h2 data-help="tt:Live signal|How far the sound rises above the room&rsquo;s background, in real time. Spikes crossing the amber line count as chime candidates.">Live signal</h2>
        <div class="scope"><canvas id="scope" width="728" height="200" role="img" aria-label="Live sound level above background, with the chime threshold"></canvas></div>
        <div class="scopectl">
          <span class="pair"><label for="ymode">Scale</label>
          <select id="ymode">
            <option value="lin">Linear (Pa)</option>
            <option value="log" selected>Log (dB)</option>
          </select></span>
          <span class="pair"><label for="yscale">Y max</label>
          <select id="yscale">
            <option value="auto" selected>Auto</option>
            <option value="0.1">0.1 Pa (74 dB)</option>
            <option value="0.3">0.3 Pa (84 dB)</option>
            <option value="1">1 Pa (94 dB)</option>
            <option value="3">3 Pa (104 dB)</option>
          </select></span>
          <span class="pair"><label for="xwin">Window</label>
          <select id="xwin" data-help="tt:Time window|How much history the plot shows. Shorter windows make individual chimes easier to see; longer ones show the pattern of a whole strike sequence.">
            <option value="400">10 s</option>
            <option value="1200" selected>30 s</option>
            <option value="2400">1 min</option>
            <option value="4800">2 min</option>
            <option value="12000">5 min</option>
            <option value="-900">15 min</option>
            <option value="-1800">30 min</option>
            <option value="-3600">1 hour</option>
          </select></span>
          <span class="win" id="winNote">25 ms bins</span>
        </div>
        <div class="grid four" style="margin-top:16px">
          <div class="stat" data-help="tt:Strikes|Strikes detected since the device last started. Each one is a row in the chime log."><div class="k">Strikes</div><div class="v" id="chimes">0</div><div class="s">since start-up</div></div>
          <div class="stat" data-help="tt:Last strike|How long ago the clock last struck, and that strike&rsquo;s peak level."><div class="k">Last strike</div><div class="v" id="last">&mdash;</div><div class="s" id="lastS">&nbsp;</div></div>
          <div class="stat" data-help="tt:Level|Current sound level at the microphone in dB SPL, and the room&rsquo;s background level. A quiet room reads about 35&ndash;45 dB."><div class="k">Level</div><div class="v" id="lvl">&mdash;</div><div class="s" id="lvlS">&nbsp;</div></div>
          <div class="stat" data-help="tt:Pitch|Strongest frequency of the last strike, and the nearest musical note. Every strike&rsquo;s pitch is saved in the chime log."><div class="k">Pitch</div><div class="v" id="pitch">&mdash;</div><div class="s" id="pitchS">&nbsp;</div></div>
        </div>
      </div>

      <div class="panel" id="evPanel">
        <h2 data-help="tt:Chime events|Each time the clock chimes, its strikes are grouped into one event (strikes closer together than the event gap) and recorded as a sound clip on the SD card, starting a quarter second before the first strike. Newest first; press &#9654; to listen, &#10515; to download the WAV.">Chime events</h2>
        <div class="recline"><span class="recdot" id="recDot"></span><span id="recMsg">checking&hellip;</span></div>
        <audio id="player" controls preload="none" hidden></audio>
        <div class="nowplay" id="nowPlay" hidden></div>
        <div class="evlist" id="evList"><div class="evempty">Loading&hellip;</div></div>
        <div class="actions" style="margin-top:12px">
          <button class="btn" id="recTest" data-help="tt:Test clip|Records 10 seconds right now and plays it back when done &mdash; the quickest way to check the microphone placement and how the clock will sound.">Record 10 s test clip</button>
          <button class="btn" id="evHist" data-help="tt:Event history|The list above holds the events since the device started. This adds this month&rsquo;s and last month&rsquo;s events from the SD card, with their recordings.">Load history</button>
          <span class="hk" id="evInfo"></span>
        </div>
        <div class="opts">
          <label data-help="tt:Recording|Save a WAV clip of every chime event on the SD card (about 20 KB per second of sound, roughly 10&ndash;40 MB a day depending on how often the clock chimes).">Record
            <select id="recEn"><option value="1">every chime</option><option value="0">off</option></select></label>
          <label data-help="tt:Event gap|Strikes closer together than this belong to one event, and the recording runs on this long after the last strike. Longer than the pause between the clock&rsquo;s strikes; shorter than the time between chimes.">Event gap
            <select id="evGap"><option>3</option><option>4</option><option>6</option><option>8</option><option>10</option><option>15</option><option>20</option></select> s</label>
          <label data-help="tt:Longest clip|A recording is split into a new file after this long.">Longest clip
            <select id="recMax"><option>30</option><option>60</option><option>90</option><option>120</option><option>180</option><option>300</option></select> s</label>
        </div>
      </div>

      <div class="panel">
        <h2>Detection tuning</h2>
        <div class="ctl">
          <label data-help="tt:Threshold|How far above the background a sound must rise to count as a chime, as a sound level. Lower = more sensitive. Aim for about two thirds of your quietest strike&rsquo;s peak.">Threshold</label>
          <input type="range" id="thr" min="50" max="100" step="0.5">
          <output id="thrV">74.0 dB</output>
        </div>
        <div class="sub" id="thrPa">0.100 Pa above background</div>
        <div class="ctl">
          <label data-help="tt:Refractory|Dead time after a detected chime before another can register. Prevents one strike counting twice; set it just under the time between strikes.">Refractory</label>
          <input type="range" id="ref" min="200" max="4000" step="100">
          <output id="refV">1200 ms</output>
        </div>
        <div class="ctl">
          <label data-help="tt:Onset rise|A chime must also jump this far above the quietest level of the previous 0.1 s. A real strike rises in a few ms; a strike that is still ringing only fades, so it can&rsquo;t count twice. 0 = off (level only). Lower it if soft strikes are missed.">Onset rise</label>
          <input type="range" id="rise" min="0" max="20" step="1">
          <output id="riseV">6 dB</output>
        </div>
        <div class="actions">
          <button class="btn primary" id="apply">Apply &amp; save</button>
          <span class="saved" id="saved">saved &#10003;</span>
        </div>
        <div class="actions" style="margin-top:14px;padding-top:14px;border-top:1px solid var(--line)">
          <button class="btn" id="learn" data-help="tt:Learn chime|Takes the pitch of the next strike (its two strongest frequencies) so the device can tell real chimes from other noises.">Learn chime</button>
          <span id="learnSt" style="font-size:13px;color:var(--dim)"></span>
          <span class="toggle-label" data-help="tt:Tone filter|When on, a sound only counts as a chime if it matches the learned pitch. Learn a chime first.">Tone filter</span>
          <select id="toneEn"><option value="0">Off</option><option value="1">On</option></select>
        </div>
      </div>

      <div class="panel">
        <h2 data-help="tt:Log viewer|The chime log on the SD card, newest first (this month and last). Use the Excel/CSV buttons for the whole history.">Log viewer</h2>
        <div class="actions" style="margin-bottom:12px">
          <button class="btn primary" id="logLoad">Load chime log</button>
          <span class="hk" id="logInfo"></span>
        </div>
        <div id="logWrap" style="max-height:330px;overflow:auto;border:1px solid var(--line);border-radius:9px;display:none">
          <table id="logTable" style="width:100%;border-collapse:collapse;font-size:12.5px"></table>
        </div>
      </div>

    </div>
    <div class="col">

      <div class="panel" id="micPanel">
        <h2 data-help="tt:Microphone|Health of the INMP441 microphone. There is no gain to set: it is a calibrated digital mic. This panel shows whether audio is arriving and how loud the room is.">Microphone</h2>
        <div class="gbar" data-help="tt:Sound level|Bar = current level. Grey mark = the background (ambient) level. Amber mark = where a chime triggers: background plus the threshold.">
          <div class="gfill" id="mFill" style="width:0%"></div>
          <div class="mk amb" id="mAmb" style="left:0"></div>
          <div class="mk trg" id="mTrg" style="left:0"></div>
        </div>
        <div class="mscale"><span>30</span><span>50</span><span>70</span><span>90</span><span>110 dB</span></div>
        <div class="gverdict dim" id="mVerdict">&mdash;</div>
        <div class="gadvice" id="mAdvice">Waiting for the microphone&hellip;</div>
        <div class="gnums">
          <span>level <b id="mLvl">&mdash;</b></span>
          <span>background <b id="mAmbV">&mdash;</b></span>
          <span>triggers at <b id="mTrgV">&mdash;</b></span>
        </div>
        <div class="gnums sep">
          <span>last strike <b id="gPeak">&mdash;</b></span>
          <span>recent <b id="gPeaks">&mdash;</b></span>
        </div>
        <div class="gnums sep" data-help="tt:Audio stream|Frames per second actually received from the mic (should match the rate), the longest stretch between reads, and audio lost in the last second because the processor was busy for longer than the ~22 ms the I2S buffer holds. Occasional small losses are harmless; a strike lasts seconds.">
          <span>frames/s <b id="mSps">&mdash;</b></span>
          <span>longest gap <b id="mGap">&mdash;</b></span>
          <span>lost <b id="mLost">&mdash;</b></span>
        </div>
        <div class="gnums sep" data-help="tt:Audio|Analysis and recordings use the frames averaged in pairs: the audio rate. Repeats = share of loud frames identical to the one before. The ESP8266 can only give the mic 32 clocks per frame (its datasheet asks for 64), so it likely sends each value twice: about 50&nbsp;% repeats. Near 0&nbsp;% means every frame is new. Either is fine; shown only while there is sound.">
          <span>audio <b id="mAud">&mdash;</b></span>
          <span>repeats <b id="mRep">&mdash;</b></span>
        </div>
        <div class="ctl" style="margin-bottom:0" data-help="tt:Channel|The ESP8266 receives two 16-bit half-words per frame; the INMP441 talks in one of them (L/R tied to GND). Auto picks the one carrying audio. Only change this if the level looks wrong.">
          <label>Channel</label>
          <select id="micCh"><option value="0">Auto</option><option value="1">Low half</option><option value="2">High half</option></select>
          <span class="hk" id="mCh"></span>
        </div>
      </div>

      <div class="panel" id="specPanel">
        <h2 data-help="tt:Spectrum|What the microphone hears, by frequency (50 Hz to 5 kHz, log scale), in dB SPL. Top: the newest tenth of a second. Bottom: a scrolling spectrogram, low notes at the bottom, brighter = louder. A chime shows as bright horizontal lines that fade; dashed lines mark the learned chime pitch.">Spectrum</h2>
        <div class="scope"><canvas id="spec" width="400" height="150" role="img" aria-label="Live spectrum: sound level by frequency"></canvas></div>
        <div class="scope specgap"><canvas id="wf" width="400" height="150" role="img" aria-label="Spectrogram: frequency over time, brighter is louder"></canvas></div>
        <div class="scopectl">
          <button class="btn primary" id="specGo" data-help="tt:Listen|Starts the live spectrum (about 4 updates a second). It stops by itself after 10 minutes, and while this tab is hidden, to spare the device.">Listen</button>
          <span class="hk" id="specPk">&mdash;</span>
          <span class="win">50 Hz&ndash;5 kHz &middot; log</span>
        </div>
      </div>

      <div class="panel" id="sdPanel">
        <h2 data-help="tt:SD card|Long-term records, saved every 30 seconds as CSV files that open in Excel straight off the card: one chime file and one event file per month, one level file per day (in a folder per month), and system.csv (boots, WiFi drops, card events). Recordings are WAV files, one folder per day.">SD card</h2>
        <div class="sdstate" id="sdState"><span class="dot"></span>
          <span class="sdtxt"><span id="sdMsg">checking&hellip;</span> <span class="nx" id="sdNext"></span></span>
          <button class="btn" id="sdRetry" hidden data-help="tt:Check now|Looks for a card right away. With no card the device checks by itself every 30 s at first, then every 5 minutes, so a card put in later is found without this button too.">Check now</button>
        </div>
        <div class="gnums">
          <span>card <b id="sdCard">&mdash;</b></span>
          <span>last write <b id="sdLast">&mdash;</b></span>
          <span>waiting <b id="sdPend">&mdash;</b></span>
        </div>
        <div class="gnums sep">
          <span>strikes saved <b id="sdCh">0</b></span>
          <span>events <b id="sdEv">0</b></span>
          <span>level rows <b id="sdLv">0</b></span>
          <span>errors <b id="sdErr">0</b></span>
        </div>
        <div class="ctl" style="margin-top:14px" data-help="tt:Level log|Records the loudest and the average (Leq) sound level for every interval, one file per day. Every second is about 2.6 MB a day, under 1 GB a year.">
          <label>Level log</label>
          <select id="lvlSec">
            <option value="0">Off</option>
            <option value="1">Every second</option>
            <option value="5">Every 5 s</option>
            <option value="10">Every 10 s</option>
            <option value="60">Every minute</option>
          </select>
          <span class="saved" id="lvlSaved">saved &#10003;</span>
        </div>
        <div class="actions">
          <button class="btn primary" onclick="dlXlsx('chimes')">Chime log (Excel)</button>
          <a class="btn" href="/api/log" download="chimes.csv">CSV</a>
          <button class="btn" onclick="dlXlsx('events')">Events (Excel)</button>
          <a class="btn" id="todayLvl" href="#">Today&rsquo;s levels</a>
          <button class="btn" id="sdBrowse">Browse files</button>
          <button class="btn" id="sdBench" data-help="tt:Speed test|Writes and reads back 256 KB on the card and reports the speed and the slowest single write. Recording needs about 20 KB/s, and the device can ride out a write that stalls for up to ~0.3 s.">Speed test</button>
        </div>
        <div class="hk" id="sdInfo" style="margin-top:10px" aria-live="polite"></div>
        <div id="sdFiles" class="files" style="display:none"></div>
      </div>

      <div class="panel">
        <h2>Device</h2>
        <div class="ctl" style="margin:0 0 14px;flex-wrap:wrap" data-help="tt:Time zone|Sets local time for log timestamps and file names, including daylight saving. Check that the device time shown matches the real time.">
          <label>Time zone</label>
          <select id="tzSel">
            <option value="PST8PDT,M3.2.0,M11.1.0">US Pacific</option>
            <option value="MST7MDT,M3.2.0,M11.1.0">US Mountain</option>
            <option value="MST7">US Arizona</option>
            <option value="CST6CDT,M3.2.0,M11.1.0">US Central</option>
            <option value="EST5EDT,M3.2.0,M11.1.0">US Eastern</option>
            <option value="AKST9AKDT,M3.2.0,M11.1.0">Alaska</option>
            <option value="HST10">Hawaii</option>
            <option value="GMT0BST,M3.5.0/1,M10.5.0">UK / Ireland</option>
            <option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe</option>
            <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Eastern Europe</option>
            <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia Eastern</option>
            <option value="NZST-12NZDT,M9.5.0,M4.1.0/3">New Zealand</option>
            <option value="UTC0">UTC</option>
          </select>
          <span class="hk" id="tzNow"></span>
          <span class="saved" id="tzSaved">saved &#10003;</span>
        </div>
        <div class="actions">
          <a class="btn" href="/update" target="_blank" data-help="tt:Firmware update|Upload a compiled .bin straight from the browser. This bypasses the Arduino IDE, espota and mDNS entirely &mdash; use Sketch &gt; Export Compiled Binary, then drop the file here.">Update firmware</a>
          <button class="btn danger" id="reset">Reset WiFi</button>
          <span class="hk" id="fw"></span>
        </div>
      </div>

    </div>
  </div>
</div>

<script>
// Canvas colours come from the theme's CSS variables so the scope always
// matches the palette (no hard-coded colours here).
const CV=(n)=>getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const $=id=>document.getElementById(id);
// Sound pressure (Pa) <-> dB SPL (re 20 uPa).
const dB=p=>p>2e-5?20*Math.log10(p/2e-5):0, paOf=d=>2e-5*Math.pow(10,d/20);
const fmtDb=p=>dB(p).toFixed(1)+' dB';
// Nearest musical note of a frequency, e.g. 523 Hz -> "C5".
const NOTES=['C','C\u266F','D','D\u266F','E','F','F\u266F','G','G\u266F','A','A\u266F','B'];
function noteOf(f){
  if(!(f>20))return '';
  const m=Math.round(69+12*Math.log2(f/440));
  return NOTES[m%12]+(Math.floor(m/12)-1);
}
let audioRate=10000;               // from /api/mic
function fmtBytes(b){
  if(b<1024)return b+' B';
  if(b<1048576)return (b/1024).toFixed(1)+' KB';
  return (b/1048576).toFixed(1)+' MB';
}

// ---- scope trace: circular buffer of recent excess levels (Pa) ----
const cv=$('scope'), cx=cv.getContext('2d');
// Bins never received are NaN and drawn as a blank gap.
const N=12000, BIN_MS=25, buf=new Float32Array(N).fill(NaN);  // ring holds up to 5 min
const coarse=new Uint8Array(N);   // 1 = patched from the device's 1-second history
const HIST_BINS=40;               // scope bins per history entry (firmware HIST_BINS)
let winBins=1200;                 // visible span (user set)
// Windows longer than the 5-minute live ring switch to HISTORY MODE: the
// device keeps one peak per second for an hour, so a long view is populated
// immediately instead of waiting (and it survives a page refresh).
let histMode=0;            // 0 = live 25ms stream, else window length in sec
let histData=null;         // Float32Array of 1-second peaks (oldest first)
function fetchHistory(){
  if(!histMode) return;
  fetch('/api/history').then(r=>r.text()).then(t=>{
    const nl=t.indexOf('\n'); if(nl<0) return;
    const hdr=t.slice(0,nl).split(','), scale=+hdr[0]||10000;
    const body=t.slice(nl+1).trim();
    if(!body){histData=new Float32Array(0);return;}
    const parts=body.split(',');
    const a=new Float32Array(parts.length);
    for(let i=0;i<parts.length;i++)a[i]=(+parts[i])/scale;
    histData=a;
  }).catch(()=>{});
}
setInterval(()=>{if(histMode)fetchHistory();},10000);
let newestSeq=-1, newestT=0;   // absolute index + arrival time of newest bin
let threshold=0.1;             // the detector's live value (Pa), from /api/state
let thrPreview=null;           // slider position not yet applied
let yMode='auto';              // 'auto' or fixed top-of-scale
let scaleType='log';           // 'lin' | 'log' — log shows quiet and loud at once
let yMax=0.2;
const LOG_DECADES=3;           // log view spans yMax down to yMax/1000 (60 dB)

// Render at native device resolution — upscaled canvases look blurry.
function fitCanvas(){
  const dpr=window.devicePixelRatio||1;
  const cssW=cv.clientWidth||728, cssH=200;
  cv.width=Math.round(cssW*dpr); cv.height=Math.round(cssH*dpr);
  cx.setTransform(dpr,0,0,dpr,0,0);
}
window.addEventListener('resize',fitCanvas);

function yOf(v,h){
  if(scaleType==='log'){
    const vmin=yMax/Math.pow(10,LOG_DECADES);
    return h-(Math.log10(Math.max(v,vmin)/vmin)/LOG_DECADES)*h;
  }
  return h-Math.min(v/yMax,1)*h;
}
function scaleLabel(){
  return scaleType==='log' ? dB(yMax).toFixed(0)+' dB · log' : yMax.toFixed(3)+' Pa';
}

function drawScope(){
  const w=cv.clientWidth||728, h=200;
  // --- scale ---
  let target;
  if(yMode==='auto'){
    let m=0;
    if(histMode&&histData&&histData.length){
      const span=Math.min(histMode,histData.length), st=histData.length-span;
      for(let i=0;i<span;i++) if(histData[st+i]>m)m=histData[st+i];
    }else{
      const lo=Math.max(0,newestSeq-winBins+1);
      for(let b=lo;b<=newestSeq;b++){const v=buf[b%N]; if(v>m)m=v;}
    }
    // Keep the threshold line(s) in view, so it is clear when nothing crosses.
    target=Math.max(0.005,threshold*1.25,(thrPreview||0)*1.25,Math.min(6,m*1.25));
  }else target=+yMode;
  yMax+=(target-yMax)*0.08;

  cx.clearRect(0,0,w,h);
  // vertical grid
  cx.strokeStyle=CV('--grid');cx.lineWidth=1;
  for(let x=0;x<=w;x+=w/8){cx.beginPath();cx.moveTo(x,0);cx.lineTo(x,h);cx.stroke();}
  // horizontal grid: quarters in linear, 20 dB steps in log
  if(scaleType==='log'){
    cx.fillStyle=CV('--axis');cx.font='10px monospace';
    for(let d=1;d<LOG_DECADES;d++){
      const gy=h-(d/LOG_DECADES)*h;
      cx.beginPath();cx.moveTo(0,gy);cx.lineTo(w,gy);cx.stroke();
      cx.fillText(dB(yMax/Math.pow(10,LOG_DECADES-d)).toFixed(0)+' dB',4,gy-3);
    }
  }else{
    for(let y=0;y<=h;y+=h/4){cx.beginPath();cx.moveTo(0,y);cx.lineTo(w,y);cx.stroke();}
  }
  // threshold line (when on-scale)
  const ty=yOf(threshold,h);
  if(ty>=0&&ty<=h){
    cx.strokeStyle=CV('--thresh');cx.setLineDash([6,5]);
    cx.beginPath();cx.moveTo(0,ty);cx.lineTo(w,ty);cx.stroke();cx.setLineDash([]);
  }
  // unapplied slider position, fainter, so tuning can be previewed
  if(thrPreview!==null&&Math.abs(thrPreview-threshold)>1e-5){
    const py=yOf(thrPreview,h);
    if(py>=0&&py<=h){
      cx.save();cx.globalAlpha=0.45;cx.strokeStyle=CV('--thresh');cx.setLineDash([2,4]);
      cx.beginPath();cx.moveTo(0,py);cx.lineTo(w,py);cx.stroke();cx.restore();
    }
  }
  // --- history mode: draw device-supplied 1-second peaks ---
  if(histMode){
    if(histData && histData.length){
      const span=Math.min(histMode,histData.length);
      const start=histData.length-span;
      cx.strokeStyle=CV('--trace');cx.lineWidth=1.5;
      cx.lineJoin='round';cx.lineCap='round';
      cx.shadowColor=CV('--traceglow');cx.shadowBlur=8;
      cx.beginPath();
      for(let i=0;i<span;i++){
        const x=(i/(span-1||1))*w, y=yOf(histData[start+i],h);
        i?cx.lineTo(x,y):cx.moveTo(x,y);
      }
      cx.stroke();cx.shadowBlur=0;
    }else{
      cx.fillStyle=CV('--axis');cx.font='12px monospace';
      cx.fillText('loading history…',12,h/2);
    }
    cx.fillStyle=CV('--axis');cx.font='12px monospace';
    cx.fillText(scaleLabel()+'  ·  1 s bins',8,14);
    requestAnimationFrame(drawScope);
    return;
  }

  // --- trace: time-interpolated scroll ---
  // Bins arrive in bursts each poll; sliding by wall-clock time between
  // arrivals turns the burst-jumps into continuous 60fps motion.
  if(newestSeq>=0){
    const pos=newestSeq+Math.min((performance.now()-newestT)/BIN_MS,40);
    const pxPerBin=w/winBins;
    cx.strokeStyle=CV('--trace');cx.lineWidth=1.5;
    cx.lineJoin='round';cx.lineCap='round';
    cx.shadowColor=CV('--traceglow');cx.shadowBlur=8;
    const first=Math.max(0,newestSeq-winBins+1);
    // pass 0: 25ms bins; pass 1: stretches patched from 1-second history, fainter
    for(let pass=0;pass<2;pass++){
      cx.globalAlpha=pass?0.45:1;
      cx.beginPath();let started=false;
      for(let b=first;b<=newestSeq;b++){
        const x=w-(pos-b)*pxPerBin;
        if(x<-2)continue;
        const v=buf[b%N];
        if(v!==v||coarse[b%N]!==pass){started=false;continue;}   // NaN: gap, lift the pen
        const y=yOf(v,h);
        started?cx.lineTo(x,y):(cx.moveTo(x,y),started=true);
      }
      cx.stroke();
    }
    cx.globalAlpha=1;cx.shadowBlur=0;
  }
  cx.fillStyle=CV('--axis');cx.font='12px monospace';
  cx.fillText(scaleLabel(),8,14);
  requestAnimationFrame(drawScope);
}
fitCanvas();
$('yscale').onchange=function(){yMode=this.value;};
$('xwin').onchange=function(){
  const v=+this.value;
  const note=$('winNote');
  if(v<0){                       // history mode: value is -seconds
    histMode=-v; histData=null; fetchHistory();
    note.textContent=(-v/60)+' min · 1 s bins · from device';
  }else{
    histMode=0; winBins=v;
    const secs=winBins*BIN_MS/1000;
    note.textContent=(secs>=60?(secs/60)+' min':secs+' s')+' · 25 ms bins';
  }
};
$('ymode').onchange=function(){scaleType=this.value;};
requestAnimationFrame(drawScope);

// ---- polling, decoupled from the render loop ----
function fmtAge(epoch,nowEpoch,valid){
  if(!epoch) return '—';
  if(!valid) return '?';
  let a=nowEpoch-epoch; if(a<0)a=0;
  if(a<90) return a+'s';
  if(a<5400) return Math.floor(a/60)+'m';
  if(a<129600) return Math.floor(a/3600)+'h';
  return Math.floor(a/86400)+'d';
}
let lastChimeCount=-1, recentPeaks=[], lastEvents=-1, lastEvStrikes=-1;
// Store bins [scopeFrom, scopeFrom+scope.length); returns how many newer bins
// the device still holds for us. Bins that aged out of the device's ring
// before we asked become NaN gaps rather than keeping stale values, and are
// queued for backfill from the device's 1-second history.
let gapLo=-1, gapHi=-1;        // span of bins awaiting backfill
let devSeq=0, bootGen=0;       // device's bin count; bumps when it reboots
function ingestScope(s){
  const arr=s.scope||[], from=s.scopeFrom, seq=s.scopeSeq;
  if(newestSeq>=0&&seq-1<newestSeq){                               // device rebooted
    buf.fill(NaN);coarse.fill(0);newestSeq=-1;gapLo=gapHi=-1;bootGen++;
  }
  devSeq=seq;
  if(newestSeq>=0&&from>newestSeq+1){
    const lo=newestSeq+1;
    for(let b=lo,end=Math.min(from,lo+N);b<end;b++){buf[b%N]=NaN;coarse[b%N]=0;}
    gapLo=gapLo<0?lo:Math.min(gapLo,lo); gapHi=Math.max(gapHi,from);
    backfill();
  }
  for(let i=0;i<arr.length;i++){buf[(from+i)%N]=arr[i];coarse[(from+i)%N]=0;}
  const last=from+arr.length-1;
  if(last>newestSeq){newestSeq=last;newestT=performance.now();}
  return seq-1-newestSeq;
}
// A throttled background tab may poll only once a minute, far longer than
// the device's 6.4s scope ring. Fill those gaps from /api/history, whose
// entries are exactly HIST_BINS scope bins each: every chime still shows at
// its true peak, at 1-second resolution (drawn fainter).
let backfilling=false;
async function backfill(){
  if(backfilling||gapLo<0) return;
  backfilling=true;
  const lo=Math.max(gapLo,newestSeq-N+1), hi=gapHi, gen=bootGen;
  gapLo=gapHi=-1;
  let ok=true;
  if(hi>lo){
    try{
      const want=Math.ceil((devSeq-lo)/HIST_BINS)+5;
      const t=await (await fetch('/api/history?n='+want)).text();
      const nl=t.indexOf('\n'), hd=t.slice(0,nl).split(',');
      const scale=+hd[0]||10000, firstBin=+hd[3], per=+hd[4];
      const body=t.slice(nl+1).trim(), v=body?body.split(','):[];
      if(gen===bootGen&&per>0)
        for(let b=Math.max(lo,newestSeq-N+1);b<hi;b++){
          if(buf[b%N]===buf[b%N]) continue;          // real 25ms data present
          const i=Math.floor((b-firstBin)/per);
          if(i>=0&&i<v.length){buf[b%N]=+v[i]/scale;coarse[b%N]=1;}
        }
    }catch(e){
      ok=false;
      if(gen===bootGen){gapLo=gapLo<0?lo:Math.min(gapLo,lo);gapHi=Math.max(gapHi,hi);}
    }
  }
  backfilling=false;
  if(gapLo>=0) setTimeout(backfill,ok?0:2000);   // gaps noted meanwhile, or retry
}
let pollBusy=false, pollTimer=0, pollFails=0;
// fetch with a time limit: a device that has gone away otherwise leaves the
// request hanging for minutes and the page showing stale "all fine" values.
function fetchT(u,ms){
  const c=new AbortController(),t=setTimeout(()=>c.abort(),ms||4000);
  return fetch(u,{signal:c.signal}).finally(()=>clearTimeout(t));
}
async function poll(){
  if(pollBusy) return;
  pollBusy=true; clearTimeout(pollTimer);
  let behind=0;
  try{
    const s=await (await fetchT('/api/state?since='+(newestSeq+1))).json();
    pollFails=0;
    behind=ingestScope(s);
    threshold=s.threshold;
    $('chimes').textContent=s.chimes;
    // Track true chime peaks so detection can be judged on real loudness.
    if(s.chimes!==lastChimeCount){
      lastChimeCount=s.chimes;
      if(s.lastPeak>0){
        recentPeaks.push(s.lastPeak); if(recentPeaks.length>6)recentPeaks.shift();
        $('gPeak').textContent=fmtDb(s.lastPeak);
        $('gPeaks').textContent=recentPeaks.map(v=>dB(v).toFixed(0)).join('  ');
      }
    }
    $('lvl').textContent=s.level>0?dB(s.level).toFixed(0)+' dB':'—';
    $('lvlS').textContent=s.ambient>0?'background '+dB(s.ambient).toFixed(0)+' dB':'\u00a0';
    $('pitch').textContent=s.lastHz>0?Math.round(s.lastHz)+' Hz':'—';
    $('pitchS').textContent=s.lastHz>0?noteOf(s.lastHz)+' · last strike':'\u00a0';
    $('last').textContent=fmtAge(s.lastChime,s.epoch,!!s.timeValid);
    $('lastS').textContent=s.lastPeak>0?'peak '+dB(s.lastPeak).toFixed(0)+' dB':'\u00a0';
    if(s.timeValid){
      $('clock').textContent=new Date(s.epoch*1000).toLocaleTimeString();
    }else{
      $('clock').textContent='NTP syncing…';
    }
    const rec=$('rec'), logging=s.sd&&s.timeValid;
    rec.className='rec '+(s.rec?'live':logging?'on':'off');
    rec.textContent=s.rec?'Recording chime':logging?'Logging':(s.sd?'Waiting for time':'Not logging');
    if(s.events!==lastEvents||s.evStrikes!==lastEvStrikes){
      lastEvents=s.events;lastEvStrikes=s.evStrikes;pollEvents();
    }
    $('fw').textContent='firmware '+s.fw+(s.heap?' · '+Math.round(s.heap/1024)+' KB free':'');
  }catch(e){
    if(++pollFails>=3){                      // ~1 s of failures: say so
      const rec=$('rec');rec.className='rec off';rec.textContent='No connection';
      $('clock').textContent='device offline';
    }
  }
  pollBusy=false;
  pollTimer=setTimeout(poll,behind>0?0:200);   // after a stall, catch up immediately
}
// A hidden tab's next poll may be throttled up to a minute away; fetch as
// soon as the page is looked at again.
document.addEventListener('visibilitychange',()=>{if(!document.hidden)poll();});

// ---- load config, wire tuning controls ----
const thr=$('thr'),thrV=$('thrV'),thrPa=$('thrPa');
const ref=$('ref'),refV=$('refV');
// The slider works in dB (sound levels are logarithmic); the device stores
// pascals. The solid plot line is the threshold the DEVICE is using (from
// /api/state); dragging the slider only previews until Apply.
function showThr(p){thrV.textContent=fmtDb(p);thrPa.textContent=p.toFixed(3)+' Pa above background';}
thr.oninput=()=>{const p=paOf(+thr.value);showThr(p);thrPreview=p;};
ref.oninput=()=>{refV.textContent=ref.value+' ms';};
const rise=$('rise'),riseV=$('riseV');
rise.oninput=()=>{riseV.textContent=+rise.value?rise.value+' dB':'off';};
function postCfg(body){
  return fetch('/api/config',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},body}).then(r=>r.json());
}
function flashSaved(id){const s=$(id);s.classList.add('show');setTimeout(()=>s.classList.remove('show'),1200);}
// Select a value, adding it as an option if it was set some other way.
function setSel(sel,v){
  v=String(v);
  if(![...sel.options].some(o=>o.value===v)){const o=document.createElement('option');o.value=v;o.textContent=v;sel.appendChild(o);}
  sel.value=v;
}
// ---- time zone (POSIX TZ rule, so daylight saving is automatic) ----
const tzSel=$('tzSel');
function showTz(c){
  if(!c.tz)return;
  if(![...tzSel.options].some(o=>o.value===c.tz)){   // a rule set by hand: list it
    const o=document.createElement('option');o.value=c.tz;o.textContent=c.tz;tzSel.appendChild(o);
  }
  tzSel.value=c.tz;
  $('tzNow').textContent=c.localTime?'device time '+c.localTime:'device time: waiting for NTP';
}
tzSel.onchange=function(){
  postCfg('tz='+encodeURIComponent(this.value)).then(j=>{
    if(!j.ok)return;
    flashSaved('tzSaved');
    return fetch('/api/config').then(r=>r.json()).then(showTz);
  });
};
fetch('/api/config').then(r=>r.json()).then(c=>{
  threshold=+c.threshold;
  thr.value=dB(threshold).toFixed(1); showThr(threshold);
  ref.value=c.refractoryMs;refV.textContent=c.refractoryMs+' ms';
  rise.value=Math.round(c.riseDb);rise.oninput();
  setSel($('recEn'),c.recEnabled);setSel($('evGap'),c.eventGapSec);setSel($('recMax'),c.recMaxSec);
  $('micCh').value=String(c.micChannel||0);
  $('lvlSec').value=String(c.levelSec);
  showTz(c);
});
$('apply').onclick=()=>{
  const p=paOf(+thr.value);
  postCfg('threshold='+p.toFixed(4)+'&refractoryMs='+ref.value+'&riseDb='+rise.value).then(()=>{
    threshold=p;thrPreview=null;flashSaved('saved');
  });
};
$('micCh').onchange=function(){postCfg('micChannel='+this.value);};
$('lvlSec').onchange=function(){postCfg('levelSec='+this.value).then(j=>{if(j.ok)flashSaved('lvlSaved');});};

const learnSt=$('learnSt');
let learned=[0,0];                 // tone filter pitches, marked on the spectrum
const learnBtn=$('learn');
learnBtn.onclick=()=>{
  learnBtn.disabled=true;
  fetch('/api/learn/start',{method:'POST'}).then(r=>r.json()).then(j=>{
    if(!j.ok){learnSt.textContent='could not start: '+(j.err||'error');learnBtn.disabled=false;return;}
    learnSt.textContent='listening — let the clock strike (60 s)';
    const iv=setInterval(async()=>{
      try{
        const st=await (await fetch('/api/learn/status')).json();
        if(st.armed)return;
        clearInterval(iv);learnBtn.disabled=false;
        learnSt.textContent=st.valid?
          'learned: '+st.f1.toFixed(0)+' Hz'+(st.f2>0?' + '+st.f2.toFixed(0)+' Hz':'')
          : 'no strike heard — try again';
        if(st.valid)learned=[st.f1,st.f2];
      }catch(e){}
    },1000);
  }).catch(()=>{learnSt.textContent='could not start';learnBtn.disabled=false;});
};
const toneEn=$('toneEn');
fetch('/api/learn/status').then(r=>r.json()).then(st=>{
  toneEn.value=st.enabled?'1':'0';
  if(st.saved1>0)learnSt.textContent='learned: '+st.saved1.toFixed(0)+' Hz'+(st.saved2>0?' + '+st.saved2.toFixed(0)+' Hz':'');
  learned=[st.saved1||0,st.saved2||0];
});
toneEn.onchange=function(){postCfg('toneEnabled='+this.value);};

// ---- Microphone health ----
const VCLS={listening:'good',starting:'dim','no data':'bad','no i2s':'bad',clipping:'warn','too quiet':'warn'};
const pct=d=>Math.max(0,Math.min(100,(d-30)/80*100));   // 30..110 dB -> 0..100%
async function pollMic(){
  try{
    const m=await (await fetch('/api/mic')).json();
    const v=$('mVerdict');
    v.textContent=m.status.charAt(0).toUpperCase()+m.status.slice(1);
    v.className='gverdict '+(VCLS[m.status]||'dim');
    $('mAdvice').textContent=m.advice;
    const trg=dB(paOf(m.ambientDb)+threshold);
    $('mFill').style.width=pct(m.levelDb)+'%';
    $('mAmb').style.left=pct(m.ambientDb)+'%';
    $('mTrg').style.left=pct(trg)+'%';
    $('mLvl').textContent=m.levelDb.toFixed(1)+' dB';
    $('mAmbV').textContent=m.ambientDb.toFixed(1)+' dB';
    $('mTrgV').textContent=trg.toFixed(1)+' dB';
    $('mSps').textContent=m.sps+' / '+Math.round(m.rate);
    $('mGap').textContent=m.gapMs.toFixed(1)+' ms';
    $('mLost').textContent=m.lostMs+' ms';
    audioRate=m.audioRate||audioRate;
    $('mAud').textContent=Math.round(m.audioRate)+' Hz';
    $('mRep').textContent=m.repeatPct<0?'(needs sound)':m.repeatPct+' %';
    const half=c=>c===2?'high':'low', other=m.channel===2?m.loDb:m.hiDb;
    $('mCh').textContent='using '+half(m.channel)+' half · other: '+
      (other<0?'constant':other.toFixed(0)+' dB');
  }catch(e){}
  setTimeout(pollMic,500);
}
pollMic();

// ---- SD card ----
function ago(s){
  if(s<60) return s+'s ago';
  if(s<5400) return Math.floor(s/60)+'m ago';
  return Math.floor(s/3600)+'h ago';
}
let sdToday='';
const plural=(n,w)=>n+' '+w+(n===1?'':'s');
const inTime=s=>s<60?s+' s':Math.ceil(s/60)+' min';
async function sdRefresh(){
  try{
    const d=await (await fetch('/api/sd')).json();
    const st=$('sdState');
    st.className='sdstate '+(d.ready?'on':'off');
    $('sdMsg').textContent=d.ready?(d.msg==='recording'?'Logging to the SD card':d.msg):d.msg;
    $('sdNext').textContent=d.ready?'':(d.retryIn?'· next check in '+inTime(d.retryIn):'· checking…');
    $('sdRetry').hidden=!!d.ready;
    $('sdCard').textContent=d.ready?(d.cardMB>=1024?(d.cardMB/1024).toFixed(1)+' GB':d.cardMB+' MB')+(d.fat===64?' exFAT':' FAT'+d.fat):'—';
    $('sdLast').textContent=d.lastWrite?ago(Math.max(0,d.now-d.lastWrite)):'—';
    $('sdPend').textContent=plural(d.pending,'strike')+', '+plural(d.levelsPending,'level row');
    $('sdCh').textContent=d.chimesSaved+(d.chimesLost?' ('+d.chimesLost+' lost)':'');
    $('sdEv').textContent=d.eventsSaved;
    $('sdLv').textContent=d.levelRows+(d.levelsDropped?' ('+d.levelsDropped+' dropped)':'');
    $('sdErr').textContent=d.errors;
    sdToday=d.today;
    const a=$('todayLvl');
    if(d.today&&d.levelSec){
      a.href='/api/sd/get?f='+encodeURIComponent('/levels/'+d.today.slice(0,7)+'/'+d.today+'.csv');
      a.style.opacity=1;
    }else{a.removeAttribute('href');a.style.opacity=.45;}
  }catch(e){}
}
async function pollSd(){await sdRefresh();setTimeout(pollSd,5000);}
pollSd();
$('sdRetry').onclick=async()=>{
  const b=$('sdRetry'),r=$('sdInfo');
  b.disabled=true;b.textContent='Checking…';
  r.textContent='';
  try{
    const d=await (await fetch('/api/sd/retry',{method:'POST'})).json();
    await sdRefresh();
    r.textContent=d.ready?'Card found.':'No card found. Check it is pushed in fully and formatted FAT32 or exFAT.';
  }catch(e){r.textContent='Could not reach the device.';}
  b.disabled=false;b.textContent='Check now';
};
const getUrl=p=>'/api/sd/get?f='+encodeURIComponent(p);
function fileRow(path,name,size,cls){
  return '<div class="fr'+(cls?' '+cls:'')+'"><a href="'+getUrl(path)+'">'+name+'</a><span class="sz">'+fmtBytes(size)+'</span></div>';
}
async function browse(){
  const box=$('sdFiles');
  if(box.style.display!=='none'){box.style.display='none';return;}
  box.style.display='';box.innerHTML='<div class="fr">loading…</div>';
  try{
    const d=await (await fetch('/api/sd/files')).json();
    if(!d.ok){box.innerHTML='<div class="fr">no SD card</div>';return;}
    let h='<div class="fh">Chimes (one file per month)</div>';
    h+=d.chimes.length?d.chimes.slice().reverse().map(f=>fileRow('/chimes/'+f[0],f[0],f[1])).join('')
                      :'<div class="fr">none yet</div>';
    h+='<div class="fh">Chime events (one file per month)</div>';
    h+=d.events.length?d.events.slice().reverse().map(f=>fileRow('/events/'+f[0],f[0],f[1])).join('')
                      :'<div class="fr">none yet</div>';
    h+='<div class="fh">Recordings (one folder per day)</div><div id="recDays"><div class="fr">loading…</div></div>';
    h+='<div class="fh">Sound levels (one file per day)</div>';
    h+=d.levels.length?d.levels.slice().reverse().map(m=>
        '<div class="fr"><button class="mo" data-m="'+m+'">▸ '+m+'</button></div><div id="m'+m+'"></div>').join('')
      :'<div class="fr">none yet</div>';
    h+='<div class="fh">System log</div>';
    h+=d.system?fileRow('/system.csv','system.csv',d.system):'<div class="fr">none yet</div>';
    box.innerHTML=h;
    box.querySelectorAll('button.mo').forEach(b=>b.onclick=()=>openMonth(b));
    loadRecDays();
  }catch(e){box.innerHTML='<div class="fr">failed to list files</div>';}
}
async function openMonth(b){
  const m=b.dataset.m, slot=$('m'+m);
  if(slot.innerHTML){slot.innerHTML='';b.textContent='▸ '+m;return;}
  b.textContent='▾ '+m;slot.innerHTML='<div class="fr sub2">loading…</div>';
  try{
    const d=await (await fetch('/api/sd/files?m='+m)).json();
    slot.innerHTML=d.ok&&d.files.length?d.files.slice().reverse().map(f=>
      fileRow('/levels/'+m+'/'+f[0],f[0],f[1],'sub2')).join(''):'<div class="fr sub2">empty</div>';
  }catch(e){slot.innerHTML='<div class="fr sub2">failed</div>';}
}
$('sdBrowse').onclick=browse;
async function loadRecDays(){
  const box=$('recDays');
  try{
    const d=await (await fetch('/api/rec/list')).json();
    const days=(d.items||[]).sort().reverse();
    box.innerHTML=days.length?days.map(x=>'<div class="fr"><button class="mo" data-d="'+x+'">▸ '+x+
      '</button></div><div id="d'+x+'"></div>').join(''):'<div class="fr">none yet</div>';
    box.querySelectorAll('button[data-d]').forEach(b=>b.onclick=()=>openDay(b));
  }catch(e){box.innerHTML='<div class="fr">failed to list recordings</div>';}
}
async function openDay(b){
  const day=b.dataset.d, slot=$('d'+day);
  if(slot.innerHTML){slot.innerHTML='';b.textContent='▸ '+day;return;}
  b.textContent='▾ '+day;slot.innerHTML='<div class="fr sub2">loading…</div>';
  try{
    const d=await (await fetch('/api/rec/list?d='+day)).json();
    const items=(d.items||[]).sort((x,y)=>x[0]<y[0]?1:-1);
    slot.innerHTML=items.length?items.map(f=>{
      const p='/recordings/'+day+'/'+f[0], secs=Math.max(0,(f[1]-512)/2/audioRate);
      return '<div class="fr sub2"><button class="pl" data-rec="'+p+'" title="Play">▶</button>'+
        '<a href="'+getUrl(p)+'&dl=1">'+f[0]+'</a><span class="sz">'+secs.toFixed(0)+' s · '+fmtBytes(f[1])+'</span></div>';
    }).join(''):'<div class="fr sub2">empty</div>';
    slot.querySelectorAll('button[data-rec]').forEach(x=>x.onclick=()=>play(x.dataset.rec));
  }catch(e){slot.innerHTML='<div class="fr sub2">failed</div>';}
}
$('sdBench').onclick=async()=>{
  const r=$('sdInfo'),b=$('sdBench');
  b.disabled=true;
  r.textContent='testing… (writes and reads 256 KB, a few seconds)';
  try{
    const d=await (await fetch('/api/sd/bench',{method:'POST'})).json();
    if(!d.ok){r.textContent='speed test failed: '+d.msg;b.disabled=false;return;}
    const verdict=d.writeKBs>=3*d.needKBs&&d.maxStallMs<300?'plenty for recording':
                  d.writeKBs>=d.needKBs?'enough for recording, but tight':'TOO SLOW for recording';
    r.textContent='write '+d.writeKBs+' KB/s · read '+d.readKBs+' KB/s · slowest write '+
      d.maxStallMs+' ms — recording needs '+d.needKBs+' KB/s: '+verdict;
  }catch(e){r.textContent='speed test failed';}
  b.disabled=false;
};

// ===== XLSX export (no dependencies) =====
// The device serves plain CSV; the browser turns it into a properly typed
// workbook. Dates/times become real Excel date/time values (ms preserved),
// numbers become numbers - so sorting, charting and formatting all work.
const _CRCT=(()=>{const t=new Uint32Array(256);for(let n=0;n<256;n++){let c=n;
 for(let k=0;k<8;k++)c=c&1?0xEDB88320^(c>>>1):c>>>1;t[n]=c>>>0;}return t;})();
function _crc32(u){let c=0xFFFFFFFF;for(let i=0;i<u.length;i++)c=_CRCT[(c^u[i])&255]^(c>>>8);return (c^0xFFFFFFFF)>>>0;}
const _enc=s=>new TextEncoder().encode(s);
const _xe=s=>String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
function _dser(y,m,d){return Date.UTC(y,m-1,d)/86400000+25569;}
function _zip(fs){const ch=[],cd=[];let off=0;
 for(const f of fs){const nm=_enc(f.name),da=_enc(f.data),cr=_crc32(da);
  const l=new DataView(new ArrayBuffer(30));l.setUint32(0,0x04034b50,true);l.setUint16(4,20,true);
  l.setUint32(14,cr,true);l.setUint32(18,da.length,true);l.setUint32(22,da.length,true);
  l.setUint16(26,nm.length,true);ch.push(new Uint8Array(l.buffer),nm,da);
  const c=new DataView(new ArrayBuffer(46));c.setUint32(0,0x02014b50,true);c.setUint16(4,20,true);
  c.setUint16(6,20,true);c.setUint32(16,cr,true);c.setUint32(20,da.length,true);
  c.setUint32(24,da.length,true);c.setUint16(28,nm.length,true);c.setUint32(42,off,true);
  cd.push(new Uint8Array(c.buffer),nm);off+=30+nm.length+da.length;}
 let cl=0;for(const c of cd)cl+=c.length;
 const e=new DataView(new ArrayBuffer(22));e.setUint32(0,0x06054b50,true);
 e.setUint16(8,fs.length,true);e.setUint16(10,fs.length,true);e.setUint32(12,cl,true);e.setUint32(16,off,true);
 const all=[...ch,...cd,new Uint8Array(e.buffer)];let tot=0;for(const a of all)tot+=a.length;
 const o=new Uint8Array(tot);let p=0;for(const a of all){o.set(a,p);p+=a.length;}return o;}
function _xlsx(cols,rows,name){
 const CN=n=>{let s='';n++;while(n>0){const m=(n-1)%26;s=String.fromCharCode(65+m)+s;n=(n-m-1)/26;}return s;};
 const SS={date:2,time:3,num:4,num4:5,int:6,num1:7,text:0};
 let sd='<row r="1">';
 cols.forEach((c,i)=>{sd+=`<c r="${CN(i)}1" s="1" t="inlineStr"><is><t>${_xe(c.h)}</t></is></c>`;});
 sd+='</row>';
 rows.forEach((r,ri)=>{sd+=`<row r="${ri+2}">`;
  cols.forEach((c,i)=>{const v=r[i];if(v===null||v===undefined||v==='')return;
   const rf=`${CN(i)}${ri+2}`,s=SS[c.t]!==undefined?SS[c.t]:0;
   sd+= c.t==='text' ? `<c r="${rf}" s="${s}" t="inlineStr"><is><t>${_xe(v)}</t></is></c>`
                     : `<c r="${rf}" s="${s}"><v>${v}</v></c>`;});
  sd+='</row>';});
 const cx=cols.map((c,i)=>`<col min="${i+1}" max="${i+1}" width="${c.w||14}" customWidth="1"/>`).join('');
 const sheet=`<?xml version="1.0" encoding="UTF-8" standalone="yes"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><dimension ref="A1"/><sheetViews><sheetView workbookViewId="0" tabSelected="1"><pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/></sheetView></sheetViews><sheetFormatPr defaultRowHeight="15"/><cols>${cx}</cols><sheetData>${sd}</sheetData><autoFilter ref="A1:${CN(cols.length-1)}${rows.length+1}"/></worksheet>`;
 const styles=`<?xml version="1.0" encoding="UTF-8" standalone="yes"?><styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><numFmts count="5"><numFmt numFmtId="164" formatCode="yyyy\-mm\-dd"/><numFmt numFmtId="165" formatCode="hh:mm:ss.000"/><numFmt numFmtId="166" formatCode="0.000"/><numFmt numFmtId="167" formatCode="0.0000"/><numFmt numFmtId="168" formatCode="0.0"/></numFmts><fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts><fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill><fill><patternFill patternType="solid"><fgColor rgb="FFF2E8D9"/><bgColor indexed="64"/></patternFill></fill></fills><borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders><cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs><cellXfs count="8"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1"/><xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/><xf numFmtId="165" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/><xf numFmtId="166" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/><xf numFmtId="167" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/><xf numFmtId="1" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/><xf numFmtId="168" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/></cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>`;
 return _zip([
  {name:'[Content_Types].xml',data:'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/></Types>'},
  {name:'_rels/.rels',data:'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>'},
  {name:'xl/workbook.xml',data:`<?xml version="1.0" encoding="UTF-8" standalone="yes"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="${_xe(name)}" sheetId="1" r:id="rId1"/></sheets></workbook>`},
  {name:'xl/_rels/workbook.xml.rels',data:'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>'},
  {name:'xl/styles.xml',data:styles},
  {name:'xl/worksheets/sheet1.xml',data:sheet}]);}

// Column schemas keyed by CSV header name -> Excel type
const XSPEC={
 date:{h:'Date',t:'date',w:12}, time:{h:'Time',t:'time',w:14},
 epoch_ms:{h:'Epoch (ms)',t:'text',w:16},
 sec_from_hour:{h:'Sec from hour',t:'num',w:15},
 peak_pa:{h:'Peak (Pa)',t:'num4',w:11},
 peak_db:{h:'Peak (dB SPL)',t:'num1',w:14},
 pitch_hz:{h:'Pitch (Hz)',t:'num1',w:11}, strike:{h:'Strike #',t:'int',w:9},
 strikes:{h:'Strikes',t:'int',w:9}, duration_s:{h:'Duration (s)',t:'num1',w:12},
 max_db:{h:'Loudest (dB SPL)',t:'num1',w:16}, recording:{h:'Recording',t:'text',w:40}};

function csvToXlsx(csv,sheetName,fname){
 const lines=csv.trim().split('\n');
 if(lines.length<2){alert('Log is empty - nothing to export.');return;}
 const head=lines[0].split(',');
 const cols=head.map(h=>XSPEC[h]||{h:h,t:'text',w:14});
 const rows=lines.slice(1).filter(l=>l.length>2).map(l=>{
  const c=l.split(',');
  return head.map((h,i)=>{
   const raw=(c[i]===undefined?'':c[i]).trim(), ty=cols[i].t;
   if(ty==='date'){const m=raw.match(/^(\d{4})-(\d{2})-(\d{2})$/);
     return m?_dser(+m[1],+m[2],+m[3]):raw;}
   if(ty==='time'){const m=raw.match(/^(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?$/);
     if(!m)return raw;
     const ms=m[4]?parseInt((m[4]+'00').slice(0,3),10):0;
     return ((+m[1])*3600+(+m[2])*60+(+m[3])+ms/1000)/86400;}
   if(ty==='num'||ty==='num4'||ty==='num1'||ty==='int'){
     const v=parseFloat(raw); return isNaN(v)?'':v;}
   return raw;});});
 const blob=new Blob([_xlsx(cols,rows,sheetName)],
   {type:'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet'});
 const a=document.createElement('a');
 a.href=URL.createObjectURL(blob); a.download=fname;
 document.body.appendChild(a); a.click();
 setTimeout(()=>{URL.revokeObjectURL(a.href);a.remove();},2000);
}
function dlXlsx(kind){
 const ev=kind==='events', info=$('sdInfo');
 info.textContent='building workbook…';
 fetch(ev?'/api/events/log':'/api/log').then(r=>r.ok?r.text():Promise.reject()).then(t=>{
   csvToXlsx(t,ev?'Chime events':'Chimes',ev?'chime-events.xlsx':'chimes.xlsx');
   info.textContent='workbook downloaded';
 }).catch(()=>{info.textContent='export failed (is the SD card in?)';});
}

// ---- log viewer ----
const MAXROWS=250;
function renderLog(text){
  const lines=text.trim().split('\n');
  if(lines.length<2||lines[0].indexOf('date')!==0){
    $('logWrap').style.display='none';
    fetch('/api/logstat').then(r=>r.json()).then(d=>{
      $('logInfo').textContent='empty — '+d.why;
    }).catch(()=>{$('logInfo').textContent='log is empty';});
    return;}
  const head=lines[0].split(',');
  let rows=lines.slice(1).filter(l=>l.length>2);
  const total=rows.length;
  rows=rows.slice(-MAXROWS).reverse();          // newest first
  let html='<tr>'+head.map(h=>'<th>'+(XSPEC[h]?XSPEC[h].h:h)+'</th>').join('')+'</tr>';
  for(const r of rows) html+='<tr>'+r.split(',').map(x=>'<td>'+x+'</td>').join('')+'</tr>';
  $('logTable').innerHTML=html;
  $('logWrap').style.display='';
  $('logInfo').textContent=total+' rows this month and last'+(total>MAXROWS?' (showing newest '+MAXROWS+')':'');
}
function loadLog(){
  $('logInfo').textContent='loading…';
  fetch('/api/log?months=2').then(r=>r.text()).then(renderLog)
    .catch(()=>{$('logInfo').textContent='failed';});
}
$('logLoad').onclick=loadLog;

// ---- Chime events + recordings ----
const player=$('player');
let playingRec='', pendingTest='', evOpen=null;
const evMap=new Map();             // epoch ms -> event (since boot + loaded history)
const recUrl=p=>getUrl(p);
const pad2=n=>String(n).padStart(2,'0');
function play(p){
  if(!p)return;
  playingRec=p;player.hidden=false;player.src=recUrl(p);player.play().catch(()=>{});
  const np=$('nowPlay');np.hidden=false;
  np.textContent='Playing '+p.replace('/recordings/','').replace('/',' · ').replace('.wav','');
  renderEvents();
}
player.onended=()=>{playingRec='';renderEvents();};
function evRow(e,now){
  const d=new Date(e.t*1000+(e.ms||0));
  const day=e.t?(new Date().toDateString()===d.toDateString()?'today':
    d.toLocaleDateString(undefined,{month:'short',day:'numeric'})):'no time';
  const tm=e.t?pad2(d.getHours())+':'+pad2(d.getMinutes())+':'+pad2(d.getSeconds()):'--:--:--';
  const m=[e.db>0?e.db.toFixed(0)+' dB':'',e.hz>0?Math.round(e.hz)+' Hz '+noteOf(e.hz):'',
           e.n>1?e.dur.toFixed(1)+' s':''].filter(Boolean).join(' · ');
  let act;
  if(now) act='<span class="na">'+(e.rec?'recording…':'')+'</span>';
  else if(e.rec) act='<button data-rec="'+e.rec+'" title="Play" aria-label="Play recording">▶</button>'+
    '<a href="'+recUrl(e.rec)+'&dl=1" title="Download WAV" aria-label="Download recording">⤓</a>';
  else act='<span class="na">no clip</span>';
  return '<div class="ev'+(now?' now':'')+(e.rec&&e.rec===playingRec?' playing':'')+'">'+
    '<div class="evt">'+tm+'<small>'+(now?'in progress':day)+'</small></div>'+
    '<div class="evn">'+e.n+'<small>'+(e.n===1?'strike':'strikes')+'</small></div>'+
    '<div class="evm">'+m+'</div><div class="eva">'+act+'</div></div>';
}
let evHtml='';
function renderEvents(){
  const list=[...evMap.values()].sort((a,b)=>(b.t*1000+b.ms)-(a.t*1000+a.ms)).slice(0,200);
  const box=$('evList');
  const h=(evOpen?evRow(evOpen,true):'')+list.map(e=>evRow(e,false)).join('')||
    '<div class="evempty">No chime events since the device started. They appear here as the clock strikes; &ldquo;Load history&rdquo; shows earlier ones.</div>';
  if(h===evHtml)return;
  evHtml=h;box.innerHTML=h;
  box.querySelectorAll('button[data-rec]').forEach(b=>b.onclick=()=>play(b.dataset.rec));
}
let evBusy=false, evTimer=0;
async function pollEvents(){
  if(evBusy)return;
  evBusy=true;clearTimeout(evTimer);
  let active=false;
  try{
    const d=await (await fetch('/api/events')).json();
    evOpen=d.open;
    for(const e of d.list) evMap.set(e.t*1000+e.ms,e);
    renderEvents();
    const r=await (await fetch('/api/rec')).json();
    active=!!r.active;
    $('recDot').className='recdot'+(active?' on':'');
    const msg=r.msg.charAt(0).toUpperCase()+r.msg.slice(1);
    $('recMsg').textContent=active?'Recording '+r.path.split('/').pop()+' · '+r.seconds.toFixed(0)+' s':
      msg+(r.clips?' · '+r.clips+' clip'+(r.clips>1?'s':'')+' since boot':'')+
      (r.lostMs?' · '+r.lostMs+' ms of audio skipped (card too slow?)':'');
    if(pendingTest&&!active){play(pendingTest);testDone();}
  }catch(e){}
  evBusy=false;
  evTimer=setTimeout(pollEvents,active||evOpen?1500:5000);
}
pollEvents();
const recTestBtn=$('recTest');
function testDone(msg){
  pendingTest='';recTestBtn.disabled=false;recTestBtn.textContent='Record 10 s test clip';
  $('evInfo').textContent=msg||'';
}
recTestBtn.onclick=()=>{
  recTestBtn.disabled=true;
  fetch('/api/rec/test',{method:'POST'}).then(r=>r.json()).then(j=>{
    if(!j.ok){testDone('could not record: '+j.err);return;}
    pendingTest=j.path;recTestBtn.textContent='Recording…';
    $('evInfo').textContent='it plays here when done';
    setTimeout(()=>{if(pendingTest===j.path)testDone('the test clip did not finish — see the recording status above');},75000);
    pollEvents();
  }).catch(()=>testDone('could not record'));
};
$('evHist').onclick=()=>{
  $('evInfo').textContent='loading…';
  fetch('/api/events/log?months=2').then(r=>r.ok?r.text():Promise.reject()).then(t=>{
    const L=t.trim().split('\n'), H=L[0].split(','), ix=k=>H.indexOf(k);
    let n=0;
    for(const l of L.slice(1)){
      const c=l.split(','), ems=+c[ix('epoch_ms')];
      if(!ems)continue;
      evMap.set(ems,{t:Math.floor(ems/1000),ms:ems%1000,n:+c[ix('strikes')],dur:+c[ix('duration_s')],
        db:+c[ix('max_db')],hz:+c[ix('pitch_hz')]||0,rec:(c[ix('recording')]||'').trim()});
      n++;
    }
    renderEvents();
    $('evInfo').textContent=n+' events this month and last';
  }).catch(()=>{$('evInfo').textContent='no event history yet (is the SD card in?)';});
};
function recCfg(){
  postCfg('recEnabled='+$('recEn').value+'&eventGapSec='+$('evGap').value+'&recMaxSec='+$('recMax').value)
    .then(j=>{
      $('evInfo').textContent=j.ok?'recording settings saved ✓':'could not save the settings';
      if(j.ok){pollEvents();setTimeout(()=>{if($('evInfo').textContent.indexOf('saved')>0)$('evInfo').textContent='';},2500);}
    }).catch(()=>{$('evInfo').textContent='could not save the settings';});
}
$('recEn').onchange=recCfg;$('evGap').onchange=recCfg;$('recMax').onchange=recCfg;

// ---- Spectrum + spectrogram ----
const sc=$('spec'),sx=sc.getContext('2d'),wf=$('wf'),wx=wf.getContext('2d');
const F_LO=50, DB_LO=0, DB_HI=100;
let specOn=false, specTimer=0, specStop=0, specSr=10000, specGen=0;
let HEAT=null;
function hexRgb(h){h=h.replace('#','');return [0,2,4].map(i=>parseInt(h.substr(i,2),16));}
function heat(v){                   // 0..1 along the theme's heat ramp
  if(!HEAT)HEAT=[0,1,2,3].map(i=>hexRgb(CV('--heat'+i)));
  v=Math.max(0,Math.min(1,v))*3;
  const i=Math.min(2,Math.floor(v)),t=v-i,a=HEAT[i],b=HEAT[i+1];
  return [a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,a[2]+(b[2]-a[2])*t];
}
function fitSpec(c){
  const dpr=window.devicePixelRatio||1,w=Math.round((c.clientWidth||400)*dpr),h=Math.round(150*dpr);
  if(c.width!==w||c.height!==h){c.width=w;c.height=h;}
}
const fFrac=f=>Math.log(f/F_LO)/Math.log((specSr/2)/F_LO);
function drawSpec(d){
  fitSpec(sc);fitSpec(wf);
  const W=sc.width,H=sc.height,dpr=window.devicePixelRatio||1,binHz=d.sr/d.n;
  sx.clearRect(0,0,W,H);
  sx.lineWidth=1;sx.strokeStyle=CV('--grid');sx.fillStyle=CV('--axis');sx.font=(10*dpr)+'px monospace';
  for(const f of [100,200,500,1000,2000,5000]){
    if(f>=specSr/2)continue;
    const x=fFrac(f)*W;sx.beginPath();sx.moveTo(x,0);sx.lineTo(x,H);sx.stroke();
    sx.fillText(f>=1000?(f/1000)+'k':String(f),x+3*dpr,H-4*dpr);
  }
  for(let db=20;db<DB_HI;db+=20){
    const y=H-(db-DB_LO)/(DB_HI-DB_LO)*H;
    sx.beginPath();sx.moveTo(0,y);sx.lineTo(W,y);sx.stroke();sx.fillText(db+' dB',4*dpr,y-3*dpr);
  }
  sx.strokeStyle=CV('--thresh');sx.setLineDash([5*dpr,4*dpr]);
  for(const f of learned) if(f>0){const x=fFrac(f)*W;sx.beginPath();sx.moveTo(x,0);sx.lineTo(x,H);sx.stroke();}
  sx.setLineDash([]);
  sx.strokeStyle=CV('--trace');sx.lineWidth=1.5*dpr;sx.lineJoin='round';
  sx.shadowColor=CV('--traceglow');sx.shadowBlur=6*dpr;
  sx.beginPath();let started=false;
  for(let k=1;k<d.db.length;k++){
    const f=k*binHz;if(f<F_LO)continue;
    const x=fFrac(f)*W,y=H-Math.max(0,Math.min(1,(d.db[k]-DB_LO)/(DB_HI-DB_LO)))*H;
    started?sx.lineTo(x,y):(sx.moveTo(x,y),started=true);
  }
  sx.stroke();sx.shadowBlur=0;
  // Spectrogram: scroll left, paint the new column at the right edge.
  const cw=Math.max(1,Math.round(2*dpr)),WH=wf.height,WW=wf.width;
  wx.drawImage(wf,-cw,0);
  const img=wx.createImageData(cw,WH),hi=specSr/2;
  for(let y=0;y<WH;y++){
    const f=F_LO*Math.pow(hi/F_LO,(WH-1-y)/(WH-1));
    const k=Math.min(d.db.length-1,Math.max(1,Math.round(f/binHz)));
    const c=heat((d.db[k]-10)/70);
    for(let x=0;x<cw;x++){const o=(y*cw+x)*4;img.data[o]=c[0];img.data[o+1]=c[1];img.data[o+2]=c[2];img.data[o+3]=255;}
  }
  wx.putImageData(img,WW-cw,0);
}
// Placeholder text until the first spectrum arrives.
function specIdle(){
  fitSpec(sc);fitSpec(wf);
  const dpr=window.devicePixelRatio||1;
  [[sx,sc,'Press Listen to see what the microphone hears'],[wx,wf,'Spectrogram · low notes at the bottom']].forEach(([x,c,t])=>{
    x.clearRect(0,0,c.width,c.height);
    x.fillStyle=CV('--axis');x.font=(12*dpr)+'px '+CV('--body');x.textAlign='center';
    x.fillText(t,c.width/2,c.height/2+4*dpr);x.textAlign='start';
  });
}
specIdle();
// `gen` ties a polling loop to one press of Listen: Stop + Listen while a
// request is in flight must not leave two loops running.
async function specPoll(gen){
  if(!specOn||gen!==specGen)return;
  if(Date.now()>specStop){specToggle(false);return;}
  if(!document.hidden){
    try{
      const d=await (await fetch('/api/spectrum')).json();
      if(d.ok&&gen===specGen){
        specSr=d.sr;drawSpec(d);
        $('specPk').textContent=d.peakHz>0?'peak '+d.peakHz.toFixed(1)+' Hz '+noteOf(d.peakHz)+' · '+d.peakDb.toFixed(0)+' dB':'—';
      }
    }catch(e){}
  }
  if(specOn&&gen===specGen)specTimer=setTimeout(()=>specPoll(gen),document.hidden?1000:250);
}
function specToggle(on){
  specOn=on;specGen++;clearTimeout(specTimer);
  $('specGo').textContent=on?'Stop':'Listen';
  if(on){specStop=Date.now()+600000;specPoll(specGen);}
  else{const pk=$('specPk');if(pk.textContent!=='—'&&!/stopped\)$/.test(pk.textContent))pk.textContent+=' (stopped)';}
}
$('specGo').onclick=()=>specToggle(!specOn);

// ---- Help mode: hover (desktop) or tap (mobile) reveals explanations ----
(function(){
  const tip=document.createElement('div'); tip.className='tip'; document.body.appendChild(tip);
  let helpOn=false;
  function parse(raw){
    // format: "tt:Title|Body text"
    let title='', body=raw;
    if(raw.indexOf('tt:')===0){ const bar=raw.indexOf('|');
      title=raw.slice(3,bar); body=raw.slice(bar+1); }
    return {title,body};
  }
  function showTip(el,x,y){
    const p=parse(el.getAttribute('data-help'));
    tip.innerHTML=(p.title?'<span class="tt">'+p.title+'</span>':'')+p.body;
    tip.classList.add('show');
    const r=tip.getBoundingClientRect();
    let nx=x+14, ny=y+14;
    if(nx+r.width>innerWidth-8) nx=innerWidth-r.width-8;
    if(ny+r.height>innerHeight-8) ny=y-r.height-14;
    tip.style.left=nx+'px'; tip.style.top=ny+'px';
  }
  function hideTip(){ tip.classList.remove('show'); }
  document.addEventListener('mousemove',e=>{
    if(!helpOn){return;}
    const el=e.target.closest('[data-help]');
    if(el) showTip(el,e.clientX,e.clientY); else hideTip();
  });
  document.addEventListener('click',e=>{
    if(!helpOn) return;
    const el=e.target.closest('[data-help]');
    if(el){ e.preventDefault(); e.stopPropagation();
      const r=el.getBoundingClientRect(); showTip(el,r.left,r.bottom);
      setTimeout(hideTip,4000);
    } else hideTip();
  },true);
  const btn=$('helpToggle');
  btn.onclick=()=>{ helpOn=!helpOn; btn.classList.toggle('on',helpOn);
    document.body.classList.toggle('help',helpOn);
    btn.textContent=helpOn?'✓ Help':'? Help';
    if(!helpOn) hideTip();
  };
})();

$('reset').onclick=()=>{
  if(!confirm('Forget WiFi and return to setup mode?'))return;
  fetch('/api/reset',{method:'POST'}).then(()=>{
    document.body.innerHTML='<div class="wrap" style="padding-top:40px">'+
      '<h1>Resetting…</h1>'+
      '<p>The device is rebooting into setup mode. Join the <b>Clockerator-Setup</b> WiFi network and open 192.168.4.1.</p></div>';
  });
};
poll();
</script>
</body></html>
)HTML";

#endif // WEBPAGES_H
