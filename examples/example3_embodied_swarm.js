var TARGETS = {
  T1: { x: 10, y: 80, type: 'debris' },
  T2: { x: 70, y: 20, type: 'survivor' },
  T3: { x: 40, y: 50, type: 'debris' }
};
var SETTLE_TICKS = 2;    // consecutive ticks (after physical arrival) a claim must hold before delivering
var DISCOVERY_TICK = 2;  // tick at which each agent publishes any target it doesn't yet see
var CONTEST_MARGIN = 5;  // must beat the current holder's score by more than this to contest
                         // an already-claimed target - unlike example2, scores here are LIVE
                         // (based on current position, which changes every tick while moving),
                         // so with no margin two agents converging on the same target endlessly
                         // leapfrog each other's claim as their live distances see-saw past one
                         // another - observed in practice before this margin was added.
var RENDER_EVERY = 50;   // how often (in ticks) to print this agent's local view of the swarm -
                         // kept infrequent because the grid is several lines: when run via
                         // run_swarm3.sh, multiple real processes' output is interleaved
                         // line-by-line, so a multi-line grid from one agent can get shredded
                         // by another agent's single-line status output between its rows. For
                         // a clean, unbroken grid, run a single agent directly instead (e.g.
                         // `./example3_embodied_swarm 1`) - the single-line claimed/ARRIVED/
                         // DELIVERED events remain the reliable signal either way.
var GRID_W = 40, GRID_H = 18;

function dist(a, b) {
  var dx = a.x - b.x, dy = a.y - b.y;
  return Math.sqrt(dx * dx + dy * dy);
}

function scoreFor(targetId) {
  return -dist(self, TARGETS[targetId]);  // higher (closer to 0) = better; based on CURRENT (live) position
}

function init() {
  var pos = platform.getPosition();
  self.x = pos.x; self.y = pos.y;
  self.myTask = null;
  self.holdTicks = 0;
  self.delivered = 0;
  console.log('agent', self.id, 'ready at (' + self.x.toFixed(1) + ',' + self.y.toFixed(1) + ')');
}

// Renders this agent's OWN local view of the swarm (self + swarm + world) as
// ASCII - no agent has a global view; this is honestly what it currently
// believes, same as everything else in fleece. Legend: @ = me, + = a live
// peer, x = unclaimed target, o = claimed target, # = delivered target.
function renderGrid() {
  var grid = [];
  for (var r = 0; r < GRID_H; r++) { var row = []; for (var c = 0; c < GRID_W; c++) row.push('.'); grid.push(row); }
  function plot(x, y, ch) {
    var c = Math.min(GRID_W - 1, Math.max(0, Math.round(x / 100 * (GRID_W - 1))));
    var r = Math.min(GRID_H - 1, Math.max(0, Math.round((100 - y) / 100 * (GRID_H - 1))));
    grid[r][c] = ch;
  }
  for (var tid in TARGETS) {
    var t = world[tid];
    if (t) plot(t.x, t.y, t.status === 'delivered' ? '#' : (t.status === 'claimed' ? 'o' : 'x'));
  }
  var peers = Object.keys(swarm);
  for (var i = 0; i < peers.length; i++) {
    var p = swarm[peers[i]];
    if (p && typeof p.x === 'number' && typeof p.y === 'number') plot(p.x, p.y, '+');
  }
  plot(self.x, self.y, '@');
  console.log('--- agent', self.id, 'view @ tick', self.uptime, '---');
  for (var row2 = 0; row2 < GRID_H; row2++) console.log(grid[row2].join(''));
}

function step() {
  self.uptime = (self.uptime || 0) + 1;

  if (self.uptime === DISCOVERY_TICK) {
    for (var id in TARGETS) {
      if (!(id in world)) {
        var t = TARGETS[id];
        world[id] = { x: t.x, y: t.y, type: t.type, status: 'unclaimed', assignedTo: null, bidScore: null };
        console.log('agent', self.id, 'published target', id);
      }
    }
  }

  if (self.myTask !== null) {
    var mine = world[self.myTask];
    if (!mine || mine.assignedTo !== self.id) {
      console.log('agent', self.id, 'lost claim on', self.myTask, '- re-entering bid pool');
      self.myTask = null;
      self.holdTicks = 0;
    } else if (mine.status === 'claimed') {
      var t = TARGETS[self.myTask];
      var pos = platform.moveToward(t.x, t.y);
      self.x = pos.x; self.y = pos.y;
      if (pos.arrived) {
        if (self.holdTicks === 0) console.log('agent', self.id, 'ARRIVED at', self.myTask);
        self.holdTicks++;
        if (self.holdTicks >= SETTLE_TICKS) {
          var delivered = Object.assign({}, mine, { status: 'delivered' });
          if (worldCompareAndSet(self.myTask, mine, delivered)) {
            console.log('agent', self.id, 'DELIVERED', self.myTask);
            self.delivered++;
            self.myTask = null;
            self.holdTicks = 0;
          }
        }
      } else {
        self.holdTicks = 0;  // still traveling - reset arrival-stability counter
      }
    }
  } else {
    var bestId = null, bestScore = -Infinity, bestCurrent = null;
    for (var tid in TARGETS) {
      var current = world[tid];
      if (!current || current.status === 'delivered') continue;
      var myScore = scoreFor(tid);
      var eligible = current.status === 'unclaimed' || myScore > current.bidScore + CONTEST_MARGIN;
      if (eligible && myScore > bestScore) {
        bestScore = myScore;
        bestId = tid;
        bestCurrent = current;
      }
    }
    if (bestId !== null) {
      var claimed = Object.assign({}, bestCurrent, { status: 'claimed', assignedTo: self.id, bidScore: bestScore });
      if (worldCompareAndSet(bestId, bestCurrent, claimed)) {
        self.myTask = bestId;
        self.holdTicks = 0;
        console.log('agent', self.id, 'claimed', bestId, 'score', bestScore.toFixed(2), '- moving...');
      }
    }
  }

  if (self.uptime % RENDER_EVERY === 0) renderGrid();

  var allDelivered = true;
  for (var k in TARGETS) {
    var c = world[k];
    if (!c || c.status !== 'delivered') { allDelivered = false; break; }
  }
  if (allDelivered && !self.announcedDone) {
    self.announcedDone = true;
    console.log('agent', self.id, ': all targets delivered');
  }
}

function destroy() {
  console.log('agent', self.id, 'shutting down, delivered', self.delivered, 'target(s)');
}
