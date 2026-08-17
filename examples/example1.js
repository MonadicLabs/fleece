function init() {
  self.role = 'coordinator';
  self.battery = 100;
  console.log('init: node', self.id, 'ready');
}

function step() {
  self.uptime = (self.uptime || 0) + 1;

  if (self.uptime === 3) {
    world.T1 = { lat: 42.1, lon: -71.05, type: 'debris', confidence: 0.87 };
    console.log('discovered target T1');
  }

  var peers = Object.keys(swarm);
  console.log('step', self.uptime, '- self.battery =', self.battery, '- peers:', peers.length);
  for (var i = 0; i < peers.length; i++) {
    var id = peers[i];
    console.log('  swarm[' + id + '] =', JSON.stringify(swarm[id]));
  }

  var knownWorld = Object.keys(world);
  console.log('  world:', JSON.stringify(knownWorld));
  for (var j = 0; j < knownWorld.length; j++) {
    var t = knownWorld[j];
    console.log('    world[' + t + '] =', JSON.stringify(world[t]));
  }
}

function destroy() {
  console.log('destroy: node shutting down after', self.uptime, 'step(s)');
}
