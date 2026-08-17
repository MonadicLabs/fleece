var TARGETS = {
  T1: { x: 10, y: 80, type: 'debris' },
  T2: { x: 70, y: 20, type: 'survivor' },
  T3: { x: 40, y: 50, type: 'debris' }
};
var SETTLE_TICKS = 5;    // consecutive ticks a claim must hold before delivering
var DISCOVERY_TICK = 2;  // tick at which each agent publishes any target it doesn't yet see

function dist(a, b) {
  var dx = a.x - b.x, dy = a.y - b.y;
  return Math.sqrt(dx * dx + dy * dy);
}

function scoreFor(targetId) {
  return -dist(self, TARGETS[targetId]);  // higher (closer to 0) = better
}

// FNV-1a-ish string hash, deterministic per node id (unlike Math.random(),
// which can seed identically across processes started in the same instant -
// observed in practice: two agents launched together landing on the exact
// same 'random' position). A node's id is already guaranteed distinct, so
// deriving position from it guarantees distinct (and reproducible) starting
// positions too.
function hashStr(s) {
  var h = 2166136261;
  for (var i = 0; i < s.length; i++) {
    h = h ^ s.charCodeAt(i);
    h = (h * 16777619) >>> 0;
  }
  return h >>> 0;
}

function init() {
  var h = hashStr(self.id);
  self.x = (h % 10007) / 10007 * 100;
  self.y = ((h >>> 12) % 10007) / 10007 * 100;
  self.myTask = null;
  self.holdTicks = 0;
  self.delivered = 0;
  console.log('agent', self.id, 'ready at (' + self.x.toFixed(1) + ',' + self.y.toFixed(1) + ')');
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
    }
  } else {
    var bestId = null, bestScore = -Infinity, bestCurrent = null;
    for (var tid in TARGETS) {
      var current = world[tid];
      if (!current || current.status === 'delivered') continue;
      var myScore = scoreFor(tid);
      var eligible = current.status === 'unclaimed' || myScore > current.bidScore;
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
        console.log('agent', self.id, 'claimed', bestId, 'score', bestScore.toFixed(2));
      }
    }
  }

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
