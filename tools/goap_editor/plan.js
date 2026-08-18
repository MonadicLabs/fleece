// fleece GOAP editor - plan model + serialization.
// Byte-exact mirror of the C planner blob format:
//   [ 'F', 'P', version(2), <CBOR body> ]
// where the body is the array [name, actions, goals, utilities, missions].
// See src/planner/fleece_planner.c (serialize_body / deserialize). Env-agnostic.
(function (root) {
    'use strict';

    const { Writer, Reader } = root.FleeceCbor;

    const MAGIC0 = 0x46, MAGIC1 = 0x50, VERSION = 2;

    // --- Model ------------------------------------------------------------
    // {
    //   name: 'Forager',
    //   actions: [{ id, name, cost, dest, dur, exec, pre:[...], eff:[...] }],
    //   goals:    [{ id, name, expr, priority, curve_id }],
    //   utilities:[{ id, name, dim, x_min, x_max, points:[{x,y}] }],
    //   missions: [{ id, name, goal_ids:[...], note }]
    // }

    const DEFAULT_MODEL = {
        name: '',
        actions: [],
        goals: [],
        utilities: [],
        missions: [],
    };

    function blankAction() {
        return { id: '', name: '', cost: '', dest: '', dur: 0, exec: '', pre: [''], eff: [''] };
    }
    function blankGoal() {
        return { id: '', name: '', expr: '', priority: 1, curve_id: '' };
    }
    function blankUtility() {
        return { id: '', name: '', dim: '', x_min: 0, x_max: 1, points: [{ x: 0, y: 1 }, { x: 1, y: 0 }] };
    }
    function blankMission() {
        return { id: '', name: '', goal_ids: [], note: '' };
    }

    // --- Serialize (byte-identical to fleece_goap_serialize) --------------

    function serialize(model) {
        const w = new Writer();
        w.out.push(MAGIC0, MAGIC1, VERSION);
        const m = Object.assign({}, DEFAULT_MODEL, model);

        w.arrayHeader(5);
        w.text(m.name);

        w.arrayHeader(m.actions.length);
        for (const a of m.actions) {
            w.arrayHeader(8);
            w.text(a.id);
            w.text(a.name);
            w.text(a.cost);
            w.text(a.dest);
            w.num(+a.dur || 0);
            w.text(a.exec);
            w.arrayHeader((a.pre || []).length);
            for (const s of a.pre || []) w.text(s);
            w.arrayHeader((a.eff || []).length);
            for (const s of a.eff || []) w.text(s);
        }

        w.arrayHeader(m.goals.length);
        for (const g of m.goals) {
            w.arrayHeader(5);
            w.text(g.id);
            w.text(g.name);
            w.text(g.expr);
            w.num(+g.priority || 0);
            w.text(g.curve_id);
        }

        w.arrayHeader(m.utilities.length);
        for (const u of m.utilities) {
            w.arrayHeader(7);
            w.text(u.id);
            w.text(u.name);
            w.text(u.dim);
            w.num(+u.x_min || 0);
            w.num(+u.x_max || 0);
            w.arrayHeader((u.points || []).length);
            for (const p of u.points || []) {
                w.arrayHeader(2);
                w.num(+p.x || 0);
                w.num(+p.y || 0);
            }
        }

        w.arrayHeader(m.missions.length);
        for (const ms of m.missions) {
            w.arrayHeader(4);
            w.text(ms.id);
            w.text(ms.name);
            w.arrayHeader((ms.goal_ids || []).length);
            for (const gid of ms.goal_ids || []) w.text(gid);
            w.text(ms.note);
        }

        return w.toBytes();
    }

    // --- Parse (mirror of fleece_goap_deserialize) ------------------------

    function parse(bytes) {
        if (!(bytes instanceof Uint8Array) || bytes.length < 3) throw new Error('blob too short');
        if (bytes[0] !== MAGIC0 || bytes[1] !== MAGIC1) throw new Error('bad magic - not a fleece plan blob');
        if (bytes[2] !== VERSION) throw new Error('unsupported plan version ' + bytes[2]);

        const r = new Reader(bytes.subarray(3));
        const m = {
            name: '',
            actions: [],
            goals: [],
            utilities: [],
            missions: [],
        };

        const top = r.arrayHeader();
        if (top !== 5) throw new Error('bad body structure');

        m.name = r.text();

        let n = r.arrayHeader();
        for (let i = 0; i < n; i++) {
            const sz = r.arrayHeader();
            if (sz !== 8) throw new Error('bad action record');
            const a = {
                id: r.text(), name: r.text(), cost: r.text(), dest: r.text(),
                dur: r.num(), exec: r.text(), pre: [], eff: [],
            };
            const np = r.arrayHeader();
            for (let j = 0; j < np; j++) a.pre.push(r.text());
            const ne = r.arrayHeader();
            for (let j = 0; j < ne; j++) a.eff.push(r.text());
            m.actions.push(a);
        }

        n = r.arrayHeader();
        for (let i = 0; i < n; i++) {
            const sz = r.arrayHeader();
            if (sz !== 5) throw new Error('bad goal record');
            m.goals.push({
                id: r.text(), name: r.text(), expr: r.text(),
                priority: r.num(), curve_id: r.text(),
            });
        }

        n = r.arrayHeader();
        for (let i = 0; i < n; i++) {
            const sz = r.arrayHeader();
            if (sz !== 7) throw new Error('bad utility record');
            const u = { id: r.text(), name: r.text(), dim: r.text(), x_min: r.num(), x_max: r.num(), points: [] };
            const np = r.arrayHeader();
            for (let j = 0; j < np; j++) {
                if (r.arrayHeader() !== 2) throw new Error('bad utility point');
                u.points.push({ x: r.num(), y: r.num() });
            }
            m.utilities.push(u);
        }

        n = r.arrayHeader();
        for (let i = 0; i < n; i++) {
            const sz = r.arrayHeader();
            if (sz !== 4) throw new Error('bad mission record');
            const ms = { id: r.text(), name: r.text(), goal_ids: [], note: '' };
            const ng = r.arrayHeader();
            for (let j = 0; j < ng; j++) ms.goal_ids.push(r.text());
            ms.note = r.text();
            m.missions.push(ms);
        }

        if (!r.done()) throw new Error('trailing bytes after body');
        return m;
    }

    // --- JSON helpers (human-editable interchange) -------------------------

    function toJSON(model) {
        return JSON.stringify(model, null, 2);
    }

    function fromJSON(text) {
        const m = JSON.parse(text);
        if (!m || typeof m !== 'object') throw new Error('plan JSON must be an object');
        if (!Array.isArray(m.actions)) throw new Error('plan JSON missing actions');
        if (!Array.isArray(m.goals)) throw new Error('plan JSON missing goals');
        if (!Array.isArray(m.utilities)) m.utilities = [];
        if (!Array.isArray(m.missions)) m.missions = [];
        m.name = m.name || '';
        for (const a of m.actions) { a.pre = a.pre || []; a.eff = a.eff || []; }
        for (const u of m.utilities) u.points = u.points || [];
        for (const ms of m.missions) ms.goal_ids = ms.goal_ids || [];
        return m;
    }

    root.FleecePlan = {
        DEFAULT_MODEL, blankAction, blankGoal, blankUtility, blankMission,
        serialize, parse, toJSON, fromJSON, VERSION,
    };
    if (typeof module !== 'undefined' && module.exports) module.exports = root.FleecePlan;
})(typeof self !== 'undefined' ? self : globalThis);
