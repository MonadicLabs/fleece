// Byte-identity self-test for the GOAP editor serializer.
//
// Builds the example4 Forager scenario in JS, serializes it, and compares the
// bytes with the blob produced by the C planner library (fleece_goap_serialize).
// Run from the repo root:
//   node tools/goap_editor/test.js [path-to-c-blob]
//
// If a C blob path is given the comparison is byte-exact; otherwise it falls
// back to a parse-format sanity round-trip.
'use strict';

const fs = require('fs');
const path = require('path');
const { isDeepStrictEqual } = require('util');

const { Writer, Reader } = require('./cbor.js');
const Plan = require('./plan.js');

// Must mirror examples/example4_goap.c / tests scenario strings exactly.
function example4Model() {
    return {
        name: 'Forager',
        actions: [
            {
                id: 'deploy', name: 'Deploy to Zone', cost: '', dest: 'zone', dur: 0,
                pre: ["function(bb){ return bb.self.location === 'base' && bb.self.battery > 25; }"],
                eff: ["function(bb){ bb.self.location = 'zone'; bb.self.battery -= 5; return bb; }"],
                exec: "function(bb, tick){ if (tick >= 3) { bb.self.location = 'zone'; bb.self.battery -= 5; return true; } return false; }",
            },
            {
                id: 'collect', name: 'Collect Food', cost: '', dest: '', dur: 0,
                pre: ["function(bb){ return bb.self.location === 'zone'; }"],
                eff: ["function(bb){ bb.self.foodCount += 1; bb.self.battery -= 8; bb.world.foodTotal += 1; return bb; }"],
                exec: "function(bb, tick){ bb.self.foodCount += 1; bb.self.battery -= 8; bb.world.foodTotal += 1; return true; }",
            },
            {
                id: 'home', name: 'Return Home', cost: '', dest: 'base', dur: 0,
                pre: ["function(bb){ return bb.self.foodCount >= 1 || bb.self.battery < 30; }"],
                eff: ["function(bb){ bb.self.location = 'base'; bb.self.battery -= 5; return bb; }"],
                exec: "function(bb, tick){ if (tick >= 3) { bb.self.location = 'base'; bb.self.battery -= 5; return true; } return false; }",
            },
            {
                id: 'rest', name: 'Rest at Base', cost: '', dest: '', dur: 0,
                pre: ["function(bb){ return bb.self.location === 'base'; }"],
                eff: ["function(bb){ bb.self.battery = 100; return bb; }"],
                exec: "function(bb, tick){ if (tick >= 2) { bb.self.battery = 100; return true; } return false; }",
            },
        ],
        goals: [
            { id: 'forage', name: 'Forage 1 food', expr: 'function(bb){ return bb.self.foodCount >= 1; }', priority: 2, curve_id: 'uFood' },
            { id: 'recharge', name: 'Battery topped up', expr: 'function(bb){ return bb.self.battery >= 95; }', priority: 2, curve_id: 'uBat' },
        ],
        utilities: [
            { id: 'uFood', name: 'Hunger', dim: 'foodCount', x_min: 0, x_max: 1, points: [{ x: 0, y: 1 }, { x: 1, y: 0 }] },
            { id: 'uBat', name: 'Battery urgency', dim: 'battery', x_min: 0, x_max: 100, points: [{ x: 0, y: 1 }, { x: 100, y: 0 }] },
        ],
        missions: [],
    };
}

function hex(buf) {
    return Array.from(buf).map((b) => b.toString(16).padStart(2, '0')).join('');
}

let failures = 0;
function check(label, cond) {
    console.log((cond ? '  ok  ' : ' FAIL ') + label);
    if (!cond) failures++;
}

const blobPath = process.argv[2];
const model = example4Model();
const jsBlob = Plan.serialize(model);

console.log('== GOAP editor serializer self-test ==');
console.log('JS blob size: ' + jsBlob.length + ' bytes');

if (blobPath) {
    const cBlob = new Uint8Array(fs.readFileSync(blobPath));
    console.log('C  blob size: ' + cBlob.length + ' bytes');
    check('byte-identical to C serializer', hex(cBlob) === hex(jsBlob));
    check('round-trip: parse(C blob) matches model',
        isDeepStrictEqual(Plan.parse(cBlob), model));
} else {
    console.log('(no C blob given - format sanity checks only)');
}

// Round-trip: serialize -> parse -> serialize must be stable.
const reparsed = Plan.parse(jsBlob);
check('serialize -> parse round-trips model', isDeepStrictEqual(reparsed, model));
const jsBlob2 = Plan.serialize(reparsed);
check('reserialize is byte-identical', hex(jsBlob2) === hex(jsBlob));

// JSON interchange.
const json = Plan.toJSON(model);
check('JSON round-trip', isDeepStrictEqual(Plan.fromJSON(json), model));

// CBOR low-level writer matches expected bytes for a couple of primitives.
const w = new Writer();
w.arrayHeader(5); w.text('Forager');
const expected = hex(new Uint8Array([0x85, 0x67, 0x46, 0x6F, 0x72, 0x61, 0x67, 0x65, 0x72]));
check('CBOR primitives encode correctly', hex(w.toBytes()) === expected);

const wr = new Writer();
wr.num(2.0); wr.num(0.5);
const expNums = hex(new Uint8Array([0x02, 0xFB, 0x3F, 0xE0, 0, 0, 0, 0, 0, 0]));
check('num: integral -> uint, fractional -> float64', hex(wr.toBytes()) === expNums);

console.log(failures ? '\n' + failures + ' FAILURE(S)' : '\nAll checks passed');
process.exit(failures ? 1 : 0);
