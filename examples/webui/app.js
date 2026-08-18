/* fleece UAV Swarm Dashboard - polls /state, paints the swarm, posts /cmd. */

'use strict';

const canvas = document.getElementById('map');
const ctx = canvas.getContext('2d');

// world units -> canvas px
const SCALE = 1.5;
const W2C = (v) => v * SCALE;

const STATE_COLORS = {
  scan: '#5eead4',
  hunt: '#fbbf24',
  return: '#fb7185',
  charge: '#a78bfa',
  idle: '#5c6578',
};

let lastState = null;
let connected = false;

// Per-unit mesh bandwidth history for the sparklines (pushed every poll).
const bwHist = {};
const BW_MAX = 120; // samples (~12s at 10Hz polling)

function fmtRate(b) {
  if (b >= 1000) return (b / 1000).toFixed(1) + 'k/s';
  return (b || 0).toFixed(0) + 'B/s';
}
function fmtBytes(b) {
  if (b >= 1048576) return (b / 1048576).toFixed(1) + 'M';
  if (b >= 1024) return (b / 1024).toFixed(0) + 'k';
  return (b || 0) + 'B';
}

// --- HTTP ---------------------------------------------------------------

async function postCmd(cmd) {
  try {
    await fetch('/cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(cmd),
    });
  } catch (e) {
    /* transient - ignore */
  }
}

async function poll() {
  try {
    const r = await fetch('/state');
    if (!r.ok) throw new Error('bad status ' + r.status);
    lastState = await r.json();
    connected = true;
  } catch (e) {
    connected = false;
  }
  (s.uavs || []).forEach((u) => {
    if (!bwHist[u.id]) bwHist[u.id] = [];
    const h = bwHist[u.id];
    h.push({ rx: u.rxRate || 0, tx: u.txRate || 0 });
    if (h.length > BW_MAX) h.shift();
  });
  paint();
}

// --- painting -----------------------------------------------------------

function paint() {
  const s = lastState;
  const overlay = document.getElementById('overlay');
  if (!s) {
    setConn(false);
    overlay.classList.remove('hidden');
    overlay.textContent = 'connecting to fleece…';
    return;
  }
  setConn(true);
  overlay.classList.add('hidden');

  document.getElementById('stat-tick').textContent = 'tick ' + s.tick;
  document.getElementById('stat-delivered').textContent = String(s.worldDelivered);

  const alive = (s.targets || []).filter((t) => t.alive).length;
  document.getElementById('stat-targets').textContent = alive + ' / ' + s.targets.length;

  drawMap(s);
  drawFleet(s);

  const btn = document.getElementById('btn-play');
  btn.textContent = s.paused ? 'Play' : 'Pause';
}

function setConn(on) {
  const dot = document.getElementById('conn-dot');
  const label = document.getElementById('conn-label');
  dot.className = 'dot ' + (on ? 'on' : 'off');
  label.textContent = on ? 'online' : 'offline';
}

function drawMap(s) {
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  // patrol zone
  const zx = W2C(s.zone.x), zy = W2C(s.zone.y), zr = W2C(s.zone.r);
  ctx.beginPath();
  ctx.setLineDash([6, 6]);
  ctx.arc(zx, zy, zr, 0, Math.PI * 2);
  ctx.strokeStyle = 'rgba(94,234,212,0.45)';
  ctx.lineWidth = 1.5;
  ctx.stroke();
  ctx.setLineDash([]);

  ctx.fillStyle = 'rgba(94,234,212,0.05)';
  ctx.beginPath();
  ctx.arc(zx, zy, zr, 0, Math.PI * 2);
  ctx.fill();

  ctx.fillStyle = 'rgba(94,234,212,0.65)';
  ctx.font = '12px ui-monospace, monospace';
  ctx.textAlign = 'center';
  ctx.fillText('PATROL ZONE', zx, zy - zr - 8);

  // base pad
  const bx = W2C(s.base.x), by = W2C(s.base.y);
  ctx.save();
  ctx.translate(bx, by);
  ctx.rotate(Math.PI / 4);
  ctx.fillStyle = 'rgba(94,234,212,0.10)';
  ctx.strokeStyle = 'rgba(94,234,212,0.55)';
  ctx.lineWidth = 1.5;
  ctx.fillRect(-14, -14, 28, 28);
  ctx.strokeRect(-14, -14, 28, 28);
  ctx.restore();
  ctx.fillStyle = 'rgba(94,234,212,0.85)';
  ctx.fillText('BASE', bx, by + 34);

  // commanded waypoint trails
  (s.uavs || []).forEach((u) => {
    if (u.wp) {
      ctx.beginPath();
      ctx.moveTo(W2C(u.x), W2C(u.y));
      ctx.lineTo(W2C(u.wp[0]), W2C(u.wp[1]));
      ctx.strokeStyle = 'rgba(151,160,181,0.25)';
      ctx.lineWidth = 1;
      ctx.setLineDash([3, 4]);
      ctx.stroke();
      ctx.setLineDash([]);
    }
  });

  const pulse = (Math.sin(performance.now() / 220) + 1) / 2; // 0..1
  (s.targets || []).forEach((t) => {
    if (!t.alive) return;
    const tx = W2C(t.x), ty = W2C(t.y);
    const r = 7 + pulse * 3;
    ctx.save();
    ctx.translate(tx, ty);
    ctx.rotate(Math.PI / 4);
    ctx.fillStyle = 'rgba(252,211,77,' + (0.55 + pulse * 0.45) + ')';
    ctx.shadowColor = 'rgba(252,211,77,0.8)';
    ctx.shadowBlur = 8 + pulse * 8;
    ctx.fillRect(-r / 2, -r / 2, r, r);
    ctx.restore();
  });

  (s.uavs || []).forEach((u) => drawUav(u, s));
}

function drawUav(u, s) {
  const color = STATE_COLORS[u.action] || STATE_COLORS.idle;
  const x = W2C(u.x), y = W2C(u.y);

  // sensor sweep while patrolling
  if (u.action === 'scan') {
    ctx.beginPath();
    ctx.arc(x, y, W2C(140), 0, Math.PI * 2);
    ctx.strokeStyle = 'rgba(94,234,212,0.12)';
    ctx.lineWidth = 1;
    ctx.stroke();
  }

  // commanded waypoint marker
  if (u.wp) {
    const wx = W2C(u.wp[0]), wy = W2C(u.wp[1]);
    ctx.beginPath();
    ctx.arc(wx, wy, 4, 0, Math.PI * 2);
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.stroke();
  }

  ctx.save();
  ctx.translate(x, y);
  ctx.rotate(u.heading);

  // rotor
  ctx.fillStyle = 'rgba(10,13,19,0.6)';
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.2;
  ctx.beginPath();
  ctx.arc(0, 0, 11, 0, Math.PI * 2);
  ctx.fill();
  ctx.stroke();

  // hull: triangle pointing along heading
  ctx.beginPath();
  ctx.moveTo(15, 0);
  ctx.lineTo(-8, 9);
  ctx.lineTo(-4, 0);
  ctx.lineTo(-8, -9);
  ctx.closePath();
  ctx.fillStyle = color;
  ctx.fill();
  ctx.strokeStyle = 'rgba(10,13,19,0.8)';
  ctx.lineWidth = 1;
  ctx.stroke();

  ctx.restore();

  // battery ring
  const frac = Math.max(0, Math.min(1, u.battery / 100));
  ctx.beginPath();
  ctx.arc(x, y, 17, -Math.PI / 2, -Math.PI / 2 + Math.PI * 2 * frac);
  ctx.strokeStyle = frac < 0.3 ? '#f87171' : '#34d399';
  ctx.lineWidth = 2;
  ctx.lineCap = 'round';
  ctx.stroke();

  // label
  ctx.fillStyle = '#e8ecf4';
  ctx.font = '600 12px ui-monospace, monospace';
  ctx.textAlign = 'center';
  ctx.fillText(u.id.toUpperCase(), x, y + 32);

  ctx.fillStyle = 'rgba(151,160,181,0.9)';
  ctx.font = '10px ui-monospace, monospace';
  ctx.fillText(u.action, x, y + 45);
}

function drawFleet(s) {
  const wrap = document.getElementById('fleet');
  wrap.innerHTML = '';
  (s.uavs || []).forEach((u) => {
    const card = document.createElement('div');
    card.className = 'uav-card';

    const state = u.action || 'idle';
    const head = document.createElement('div');
    head.className = 'head';
    head.innerHTML =
      '<span class="name">' + u.id.toUpperCase() + ' <span class="id">#' + state + '</span></span>' +
      '<span class="state ' + state + '">' + state + '</span>';
    card.appendChild(head);

    const goal = document.createElement('div');
    goal.className = 'goal';
    goal.innerHTML = 'goal <b>' + u.goal + '</b>';
    card.appendChild(goal);

    const batt = document.createElement('div');
    batt.className = 'batt';
    const frac = Math.max(0, Math.min(1, u.battery / 100));
    const bar = document.createElement('i');
    bar.style.width = (frac * 100).toFixed(0) + '%';
    if (frac < 0.3) bar.className = 'low';
    batt.appendChild(bar);
    card.appendChild(batt);

    const meta = document.createElement('div');
    meta.className = 'meta';
    meta.innerHTML =
      '<span class="pos">' + u.x.toFixed(0) + ',' + u.y.toFixed(0) + '</span>' +
      '<span>' + u.battery.toFixed(1) + ' V · ' + (u.cargo ? '● cargo' : '') + (u.detect ? ' ◎ detect' : '') + '</span>';
    card.appendChild(meta);

    const bw = document.createElement('div');
    bw.className = 'bw';
    const cv = document.createElement('canvas');
    cv.width = 132;
    cv.height = 34;
    bw.appendChild(cv);
    drawBw(cv, u, bwHist[u.id] || []);
    const txt = document.createElement('div');
    txt.className = 'bwtxt';
    txt.innerHTML =
      '<span class="rx">▼ ' + fmtRate(u.rxRate) + '</span>' +
      '<span class="tx">▲ ' + fmtRate(u.txRate) + '</span>' +
      '<span class="tot">' + fmtBytes(u.rxBytes) + '/' + fmtBytes(u.txBytes) + '</span>';
    bw.appendChild(txt);
    card.appendChild(bw);

    wrap.appendChild(card);
  });
}

// Bandwidth sparkline: RX (teal) and TX (amber) bytes/sec over recent polls.
function drawBw(cv, u, hist) {
  const g = cv.getContext('2d');
  const W = cv.width, H = cv.height;
  g.clearRect(0, 0, W, H);
  g.strokeStyle = 'rgba(151,160,181,0.25)';
  g.lineWidth = 1;
  g.beginPath();
  g.moveTo(0, H - 1);
  g.lineTo(W, H - 1);
  g.stroke();
  if (hist.length < 2) return;
  const max = Math.max(1024, ...hist.map((p) => Math.max(p.rx, p.tx)));
  const step = W / (BW_MAX - 1);
  for (const key of ['rx', 'tx']) {
    g.beginPath();
    hist.forEach((p, i) => {
      const x = W - (BW_MAX - 1 - i) * step;
      const y = H - 2 - (p[key] / max) * (H - 4);
      if (i === 0) g.moveTo(x, y);
      else g.lineTo(x, y);
    });
    g.strokeStyle = key === 'rx' ? '#5eead4' : '#fbbf24';
    g.lineWidth = 1.5;
    g.stroke();
  }
}

// --- controls -----------------------------------------------------------

function wireControls() {
  const btnPlay = document.getElementById('btn-play');
  btnPlay.addEventListener('click', () => {
    const paused = lastState && lastState.paused;
    postCmd({ cmd: paused ? 'play' : 'pause' });
  });

  document.getElementById('btn-step').addEventListener('click', () => postCmd({ cmd: 'step' }));
  document.getElementById('btn-reset').addEventListener('click', () => postCmd({ cmd: 'reset' }));

  const speed = document.getElementById('speed');
  const speedLabel = document.getElementById('speed-label');
  speed.addEventListener('input', () => {
    speedLabel.textContent = (speed.value / 100).toFixed(1) + '×';
  });
  speed.addEventListener('change', () => {
    postCmd({ cmd: 'speed', speed: speed.value / 100 });
  });
}

wireControls();
poll();
setInterval(poll, 100); // 10 Hz - the runtime ticks at 10 Hz