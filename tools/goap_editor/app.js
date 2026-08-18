// fleece GOAP Planner Studio - editor UI.
// Depends on cbor.js + plan.js (loaded before this file as plain scripts).
(function () {
    'use strict';

    const P = globalThis.FleecePlan;

    // --- tiny DOM helpers -------------------------------------------------

    const $ = (sel) => document.querySelector(sel);
    // `content` is a lazy handle to the <section> so this file can also be
    // required under Node for self-tests (no DOM access at load time).
    const content = new Proxy({}, {
        get: (_t, k) => {
            const node = document.getElementById('content');
            const v = node[k];
            return typeof v === 'function' ? v.bind(node) : v;
        },
        set: (_t, k, v) => { document.getElementById('content')[k] = v; return true; },
    });
    const ACCENTS = {
        actions: 'var(--accent)',
        goals: 'var(--amber)',
        utilities: 'var(--violet)',
        missions: 'var(--rose)',
    };
    const TAB_LABELS = {
        actions: 'Action', goals: 'Goal', utilities: 'Utility', missions: 'Mission',
    };

    function el(tag, attrs, children) {
        const node = document.createElement(tag);
        if (attrs) {
            for (const [k, v] of Object.entries(attrs)) {
                if (k === 'class') node.className = v;
                else if (k === 'text') node.textContent = v;
                else if (k === 'html') node.innerHTML = v;
                else if (k.startsWith('on') && typeof v === 'function') node.addEventListener(k.slice(2), v);
                else node.setAttribute(k, v);
            }
        }
        if (children) {
            for (const c of [].concat(children)) {
                if (c == null) continue;
                node.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
            }
        }
        return node;
    }

    const ICON = {
        chevron: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="m6 9 6 6 6-6"/></svg>',
        up: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m18 15-6-6-6 6"/></svg>',
        down: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m6 9 6 6 6-6"/></svg>',
        trash: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18M8 6V4a1 1 0 0 1 1-1h6a1 1 0 0 1 1 1v2m3 0v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/></svg>',
        plus: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>',
    };

    // --- state ------------------------------------------------------------

    let state = null;
    let activeTab = 'actions';
    let expanded = new Set();
    let toastTimer = null;
    let importKind = 'json';

    const STORE_KEY = 'fleece.goap.editor.v1';

    // --- model seed: exact example4 scenario (byte-identical export) ------

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
            missions: [
                { id: 'm1', name: 'Gather and RTB', goal_ids: ['forage', 'recharge'], note: 'scan, collect one food, return' },
            ],
        };
    }

    // --- validation -------------------------------------------------------

    function jsValidate(kind, src) {
        const s = src == null ? '' : String(src);
        if (!s.trim()) return { ok: true, error: '' };  // unset = neutral
        try {
            // Mirror the bridge: sources are compiled wrapped in parens
            // (src/embedded/fleece_goap_js.c compile_goap_fn).
            if (kind === 'exec') new Function('bb', 'tick', '(' + s + ')');
            else new Function('bb', '(' + s + ')');
            return { ok: true, error: '' };
        } catch (e) {
            return { ok: false, error: e.message };
        }
    }

    function countErrors() {
        let n = 0;
        for (const a of state.actions) {
            for (const k of ['cost', 'exec']) if (!jsValidate(k, a[k]).ok) n++;
            for (const s of a.pre) if (!jsValidate('pre', s).ok) n++;
            for (const s of a.eff) if (!jsValidate('eff', s).ok) n++;
        }
        for (const g of state.goals) if (!jsValidate('goal', g.expr).ok) n++;
        return n;
    }

    function dupCount(list) {
        const seen = new Set();
        let dups = 0;
        for (const item of list) {
            const id = item.id.trim();
            if (!id) continue;
            if (seen.has(id)) dups++;
            seen.add(id);
        }
        return dups;
    }

    // --- persistence ------------------------------------------------------

    function save() {
        try { localStorage.setItem(STORE_KEY, JSON.stringify(state)); } catch (e) { /* ignore */ }
    }

    function load() {
        try {
            const raw = localStorage.getItem(STORE_KEY);
            if (raw) {
                const m = JSON.parse(raw);
                if (m && Array.isArray(m.actions)) {
                    state = P.fromJSON(raw);
                    return true;
                }
            }
        } catch (e) { /* ignore */ }
        return false;
    }

    // --- header / badges --------------------------------------------------

    function fmtBytes(n) {
        if (n < 1024) return n + ' B';
        if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
        return (n / (1024 * 1024)).toFixed(2) + ' MB';
    }

    function updateBadges() {
        const blob = P.serialize(state);
        $('#size-label').textContent = fmtBytes(blob.length);
        $('#size-bytes').textContent = '(' + blob.length + ' B)';

        const errs = countErrors();
        const pill = $('#validity-pill');
        const dot = $('#validity-dot');
        const label = $('#validity-label');
        if (errs === 0) {
            pill.classList.remove('err');
            pill.classList.add('ok');
            dot.classList.add('ok');
            dot.classList.remove('err');
            label.textContent = 'all JS valid';
        } else {
            pill.classList.remove('ok');
            pill.classList.add('err');
            dot.classList.remove('ok');
            dot.classList.add('err');
            label.textContent = errs + ' JS error' + (errs === 1 ? '' : 's');
        }
    }

    function updateCounts() {
        $('#count-actions').textContent = state.actions.length;
        $('#count-goals').textContent = state.goals.length;
        $('#count-utilities').textContent = state.utilities.length;
        $('#count-missions').textContent = state.missions.length;
        const dups = { actions: dupCount(state.actions), goals: dupCount(state.goals), utilities: dupCount(state.utilities), missions: dupCount(state.missions) };
        for (const tab of ['actions', 'goals', 'utilities', 'missions']) {
            const c = $('#count-' + tab);
            if (dups[tab] > 0) {
                c.classList.add('dup');
                c.title = dups[tab] + ' duplicate id(s)';
            } else {
                c.classList.remove('dup');
                c.title = '';
            }
        }
    }

    // --- status dot for a code field --------------------------------------

    function dotFor(kind, src) {
        const v = jsValidate(kind, src);
        const dot = el('span', { class: 'dot' });
        if (v.ok && src != null && String(src).trim()) dot.classList.add('ok');
        else if (!v.ok) { dot.classList.add('err'); dot.title = v.error; }
        return dot;
    }

    // --- code source editor -----------------------------------------------

    function sourceEditor(kind, tag, value, onChange) {
        const wrap = el('div', { class: 'source-row' });
        const ta = el('textarea', {
            class: 'code' + (jsValidate(kind, value).ok ? '' : ' err'),
            rows: Math.max(2, Math.min(7, String(value || '').split('\n').length)),
            spellcheck: 'false',
        });
        ta.value = value || '';
        ta.addEventListener('input', () => {
            ta.classList.toggle('err', !jsValidate(kind, ta.value).ok);
            onChange(ta.value);
            updateBadges();
            save();
        });
        const label = el('span', { class: 'code-tag', text: tag });
        const actions = el('div', { class: 'source-actions' });
        const copy = iconButton('copy', 'Copy', () => {
            copyText(ta.value);
            toast('Copied ' + tag + ' source', 'ok');
        });
        actions.appendChild(copy);
        wrap.append(label, ta, actions);
        return wrap;
    }

    function iconButton(name, title, fn, cls) {
        const b = el('button', { class: 'icon-btn ' + (cls || ''), title: title, html: ICON[name] });
        b.addEventListener('click', (e) => { e.stopPropagation(); fn(); });
        return b;
    }

    // --- utility curve SVG ------------------------------------------------

    function curveSVG(util) {
        const W = 600, H = 78, PAD = 6;
        const pts = (util.points || []).filter((p) => p && isFinite(p.x) && isFinite(p.y));
        if (!pts.length) {
            const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
            svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
            const t = document.createElementNS('http://www.w3.org/2000/svg', 'text');
            t.setAttribute('x', W / 2); t.setAttribute('y', H / 2);
            t.setAttribute('text-anchor', 'middle'); t.setAttribute('fill', '#5c6578');
            t.setAttribute('font-size', '11'); t.textContent = 'no points';
            svg.appendChild(t);
            return svg;
        }
        const xMin = util.x_min, xMax = util.x_max || 1;
        const span = (xMax - xMin) || 1;
        let yMax = -Infinity, yMin = Infinity;
        for (const p of pts) { yMax = Math.max(yMax, p.y); yMin = Math.min(yMin, p.y); }
        if (!isFinite(yMax) || !isFinite(yMin)) { yMin = 0; yMax = 1; }
        if (yMax === yMin) { yMax += 1; }
        const X = (x) => PAD + ((x - xMin) / span) * (W - 2 * PAD);
        const Y = (y) => H - PAD - ((y - yMin) / (yMax - yMin)) * (H - 2 * PAD);

        const ns = 'http://www.w3.org/2000/svg';
        const svg = document.createElementNS(ns, 'svg');
        svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
        svg.setAttribute('preserveAspectRatio', 'none');

        const area = document.createElementNS(ns, 'path');
        let d = 'M ' + X(pts[0].x) + ' ' + Y(pts[0].y);
        for (let i = 1; i < pts.length; i++) d += ' L ' + X(pts[i].x) + ' ' + Y(pts[i].y);
        d += ' L ' + X(pts[pts.length - 1].x) + ' ' + (H - PAD) + ' L ' + X(pts[0].x) + ' ' + (H - PAD) + ' Z';
        area.setAttribute('d', d);
        area.setAttribute('fill', 'rgba(167,139,250,0.16)');

        const line = document.createElementNS(ns, 'path');
        let dl = 'M ' + X(pts[0].x) + ' ' + Y(pts[0].y);
        for (let i = 1; i < pts.length; i++) dl += ' L ' + X(pts[i].x) + ' ' + Y(pts[i].y);
        line.setAttribute('d', dl);
        line.setAttribute('fill', 'none');
        line.setAttribute('stroke', '#a78bfa');
        line.setAttribute('stroke-width', '2');
        line.setAttribute('stroke-linejoin', 'round');

        svg.appendChild(area);
        svg.appendChild(line);

        for (let i = 0; i < pts.length; i++) {
            const c = document.createElementNS(ns, 'circle');
            c.setAttribute('cx', X(pts[i].x));
            c.setAttribute('cy', Y(pts[i].y));
            c.setAttribute('r', '3');
            c.setAttribute('fill', i === 0 ? '#fbbf24' : '#a78bfa');
            svg.appendChild(c);
        }
        return svg;
    }

    // --- field label helper ------------------------------------------------

    function fieldLabel(text, dot) {
        const l = el('label');
        if (dot) l.appendChild(dot);
        l.appendChild(document.createTextNode(text));
        return l;
    }

    // --- render actions ---------------------------------------------------

    function renderActions() {
        const accent = ACCENTS.actions;
        state.actions.forEach((a, i) => {
            expanded.add('a' + i);
            const card = el('article', { class: 'card' });
            card.style.setProperty('--card-accent', accent);

            // header
            const idInput = el('input', {
                class: 'id-input', type: 'text', spellcheck: 'false', value: a.id,
                title: 'Action id (referenced by plans)',
            });
            idInput.addEventListener('input', () => { a.id = idInput.value; updateCounts(); save(); });
            const nameInput = el('input', {
                class: 'name-input', type: 'text', spellcheck: 'false', value: a.name,
                title: 'Human-readable action name',
            });
            nameInput.addEventListener('input', () => { a.name = nameInput.value; save(); });

            const title = el('div', { class: 'card-title' }, [idInput, nameInput]);
            const actions = el('div', { class: 'card-actions' }, [
                iconButton('up', 'Move up', () => move('actions', i, -1)),
                iconButton('down', 'Move down', () => move('actions', i, 1)),
                iconButton('trash', 'Delete action', () => remove('actions', i), 'danger'),
            ]);
            const chev = el('span', { class: 'chevron', html: ICON.chevron });
            const head = el('div', { class: 'card-header' }, [chev, title, actions]);
            head.addEventListener('click', (e) => {
                if (e.target.closest('.card-actions')) return;
                card.classList.toggle('collapsed');
                if (card.classList.contains('collapsed')) expanded.delete('a' + i);
                else expanded.add('a' + i);
            });
            card.appendChild(head);

            // body
            const body = el('div', { class: 'card-body' });

            const grid = el('div', { class: 'field-grid' });
            const dest = el('input', { type: 'text', spellcheck: 'false', value: a.dest, placeholder: 'zone' });
            dest.addEventListener('input', () => { a.dest = dest.value; save(); });
            grid.appendChild(el('div', { class: 'field' }, [fieldLabel('Dest'), dest, el('div', { class: 'field-help', text: 'planner hint only' })]));

            const dur = el('input', { type: 'number', step: 'any', value: a.dur, placeholder: '0' });
            dur.addEventListener('input', () => { a.dur = parseFloat(dur.value) || 0; updateBadges(); save(); });
            grid.appendChild(el('div', { class: 'field' }, [fieldLabel('Dur (deprecated)'), dur, el('div', { class: 'field-help', text: 'timing hint; exec() owns timing now' })]));
            body.appendChild(grid);

            const costField = el('div', { class: 'code-field' }, [
                el('div', { class: 'field-label' }, [dotFor('cost', a.cost), el('span', { text: 'Cost' }), el('span', { style: 'font-weight:400;text-transform:none;letter-spacing:0;color:var(--dim);', text: ' · bb → number (empty = 1)' })]),
                sourceEditor('cost', 'cost', a.cost, (v) => { a.cost = v; }),
            ]);
            body.appendChild(costField);

            const execField = el('div', { class: 'code-field' }, [
                el('div', { class: 'field-label' }, [dotFor('exec', a.exec), el('span', { text: 'Exec · the real action body' }), el('span', { style: 'font-weight:400;text-transform:none;letter-spacing:0;color:var(--dim);', text: 'exec(bb, tick) → true when done. Empty = no-op action.' })]),
                sourceEditor('exec', 'exec', a.exec, (v) => { a.exec = v; }),
            ]);
            body.appendChild(execField);

            body.appendChild(sourceList('pre', a, 'Preconditions', 'Pre must hold before the planner picks this action.', accent));
            body.appendChild(sourceList('eff', a, 'Effects', 'Declared outcome — planner heuristic only, never applied by the executor.', accent));

            card.appendChild(body);
            content.appendChild(card);
        });

        const add = el('button', { class: 'add-block', text: 'Add ' + TAB_LABELS.actions });
        add.innerHTML = ICON.plus + 'Add ' + TAB_LABELS.actions;
        add.addEventListener('click', () => { state.actions.push(P.blankAction()); save(); render(); });
        content.appendChild(add);
    }

    function sourceList(kind, a, title, help, accent) {
        const wrap = el('div', { class: 'code-field' });
        wrap.appendChild(el('div', { class: 'field-label' }, [
            el('span', { text: title }),
            el('span', { style: 'font-weight:400;text-transform:none;letter-spacing:0;color:var(--dim);', text: ' · ' + help }),
        ]));
        const list = a[kind];
        list.forEach((src, j) => {
            const srcWrap = el('div', { class: 'source-row' });
            const ta = el('textarea', {
                class: 'code' + (jsValidate(kind, src).ok ? '' : ' err'),
                rows: Math.max(2, Math.min(6, String(src || '').split('\n').length)),
                spellcheck: 'false',
            });
            ta.value = src || '';
            ta.addEventListener('input', () => {
                ta.classList.toggle('err', !jsValidate(kind, ta.value).ok);
                list[j] = ta.value;
                updateBadges();
                save();
            });
            const label = el('span', { class: 'code-tag', text: kind });
            const act = el('div', { class: 'source-actions' }, [
                iconButton('up', 'Move source up', () => {
                    if (j > 0) { [list[j - 1], list[j]] = [list[j], list[j - 1]]; save(); render(); }
                }),
                iconButton('trash', 'Remove source', () => {
                    list.splice(j, 1);
                    if (!list.length) list.push('');
                    save(); render();
                }, 'danger'),
            ]);
            srcWrap.append(label, ta, act);
            wrap.appendChild(srcWrap);
        });
        const addBtn = el('button', { class: 'add-source', html: ICON.plus + 'Add ' + kind + ' source' });
        addBtn.addEventListener('click', () => { list.push(''); save(); render(); });
        wrap.appendChild(addBtn);
        return wrap;
    }

    // --- render goals ------------------------------------------------------

    function renderGoals() {
        const accent = ACCENTS.goals;
        const curveIds = state.utilities.map((u) => u.id);
        state.goals.forEach((g, i) => {
            expanded.add('g' + i);
            const card = el('article', { class: 'card' });
            card.style.setProperty('--card-accent', accent);

            const idInput = el('input', { class: 'id-input', type: 'text', spellcheck: 'false', value: g.id, title: 'Goal id' });
            idInput.addEventListener('input', () => { g.id = idInput.value; updateCounts(); save(); });
            const nameInput = el('input', { class: 'name-input', type: 'text', spellcheck: 'false', value: g.name });
            nameInput.addEventListener('input', () => { g.name = nameInput.value; save(); });

            const chev = el('span', { class: 'chevron', html: ICON.chevron });
            const head = el('div', { class: 'card-header' }, [
                chev,
                el('div', { class: 'card-title' }, [idInput, nameInput]),
                el('div', { class: 'card-actions' }, [
                    iconButton('up', 'Move up', () => move('goals', i, -1)),
                    iconButton('down', 'Move down', () => move('goals', i, 1)),
                    iconButton('trash', 'Delete goal', () => remove('goals', i), 'danger'),
                ]),
            ]);
            head.addEventListener('click', (e) => {
                if (e.target.closest('.card-actions')) return;
                card.classList.toggle('collapsed');
                if (card.classList.contains('collapsed')) expanded.delete('g' + i);
                else expanded.add('g' + i);
            });
            card.appendChild(head);

            const body = el('div', { class: 'card-body' });
            const grid = el('div', { class: 'field-grid' });
            const prio = el('input', { type: 'number', step: 'any', value: g.priority });
            prio.addEventListener('input', () => { g.priority = parseFloat(prio.value) || 0; save(); });
            grid.appendChild(el('div', { class: 'field' }, [fieldLabel('Priority'), prio, el('div', { class: 'field-help', text: 'multiplies utility during goal selection' })]));

            const curve = el('input', { type: 'text', spellcheck: 'false', value: g.curve_id, placeholder: 'uFood', list: 'curve-ids' });
            curve.addEventListener('input', () => { g.curve_id = curve.value; save(); });
            grid.appendChild(el('div', { class: 'field' }, [fieldLabel('Utility curve'), curve, el('div', { class: 'field-help', text: 'id of a utility below' })]));
            body.appendChild(grid);

            const exprField = el('div', { class: 'code-field' }, [
                el('div', { class: 'field-label' }, [dotFor('goal', g.expr), el('span', { text: 'Satisfied when' }), el('span', { style: 'font-weight:400;text-transform:none;letter-spacing:0;color:var(--dim);', text: ' · bb → bool' })]),
                sourceEditor('goal', 'goal', g.expr, (v) => { g.expr = v; }),
            ]);
            body.appendChild(exprField);
            card.appendChild(body);
            content.appendChild(card);
        });

        if (curveIds.length) {
            const dl = el('datalist', { id: 'curve-ids' });
            for (const id of curveIds) dl.appendChild(el('option', { value: id }));
            content.appendChild(dl);
        }

        const add = el('button', { class: 'add-block', html: ICON.plus + 'Add ' + TAB_LABELS.goals });
        add.addEventListener('click', () => { state.goals.push(P.blankGoal()); save(); render(); });
        content.appendChild(add);
    }

    // --- render utilities --------------------------------------------------

    function renderUtilities() {
        const accent = ACCENTS.utilities;
        state.utilities.forEach((u, i) => {
            expanded.add('u' + i);
            const card = el('article', { class: 'card' });
            card.style.setProperty('--card-accent', accent);

            const idInput = el('input', { class: 'id-input', type: 'text', spellcheck: 'false', value: u.id, title: 'Utility id' });
            idInput.addEventListener('input', () => { u.id = idInput.value; updateCounts(); save(); });
            const nameInput = el('input', { class: 'name-input', type: 'text', spellcheck: 'false', value: u.name });
            nameInput.addEventListener('input', () => { u.name = nameInput.value; save(); });

            const chev = el('span', { class: 'chevron', html: ICON.chevron });
            const head = el('div', { class: 'card-header' }, [
                chev,
                el('div', { class: 'card-title' }, [idInput, nameInput]),
                el('div', { class: 'card-actions' }, [
                    iconButton('up', 'Move up', () => move('utilities', i, -1)),
                    iconButton('down', 'Move down', () => move('utilities', i, 1)),
                    iconButton('trash', 'Delete utility', () => remove('utilities', i), 'danger'),
                ]),
            ]);
            head.addEventListener('click', (e) => {
                if (e.target.closest('.card-actions')) return;
                card.classList.toggle('collapsed');
                if (card.classList.contains('collapsed')) expanded.delete('u' + i);
                else expanded.add('u' + i);
            });
            card.appendChild(head);

            const body = el('div', { class: 'card-body' });
            const grid = el('div', { class: 'field-grid' });
            const dim = el('input', { type: 'text', spellcheck: 'false', value: u.dim, placeholder: 'battery' });
            dim.addEventListener('input', () => { u.dim = dim.value; save(); });
            grid.appendChild(el('div', { class: 'field' }, [fieldLabel('Dimension'), dim, el('div', { class: 'field-help', text: 'self.<dim> drives the curve' })]));

            const xmin = el('input', { type: 'number', step: 'any', value: u.x_min });
            xmin.addEventListener('input', () => { u.x_min = parseFloat(xmin.value) || 0; redrawCurve(i); save(); });
            grid.appendChild(el('div', { class: 'field' }, [fieldLabel('X min'), xmin]));

            const xmax = el('input', { type: 'number', step: 'any', value: u.x_max });
            xmax.addEventListener('input', () => { u.x_max = parseFloat(xmax.value) || 1; redrawCurve(i); save(); });
            grid.appendChild(el('div', { class: 'field' }, [fieldLabel('X max'), xmax]));
            body.appendChild(grid);

            const curveWrap = el('div', { class: 'curve-wrap' }, [
                el('div', { class: 'field-label' }, [el('span', { text: 'Utility curve' }), el('span', { style: 'font-weight:400;text-transform:none;letter-spacing:0;color:var(--dim);', text: ' · utility of the dimension value (0…1)' })]),
                el('div', { class: 'curve' }),
            ]);
            body.appendChild(curveWrap);

            const points = el('div', { class: 'points' });
            u.points.forEach((p, j) => {
                const row = el('div', { class: 'point-row' });
                const x = el('input', { type: 'number', step: 'any', value: p.x });
                x.addEventListener('input', () => { p.x = parseFloat(x.value) || 0; redrawCurve(i); save(); });
                const y = el('input', { type: 'number', step: 'any', value: p.y });
                y.addEventListener('input', () => { p.y = parseFloat(y.value) || 0; redrawCurve(i); save(); });
                row.append(el('span', { class: 'axis', text: 'x' }), x, el('span', { class: 'axis', text: 'y' }), y,
                    iconButton('trash', 'Remove point', () => { u.points.splice(j, 1); save(); render(); }, 'danger'));
                points.appendChild(row);
            });
            body.appendChild(points);

            const add = el('button', { class: 'add-source', html: ICON.plus + 'Add point' });
            add.addEventListener('click', () => { u.points.push({ x: u.x_max, y: 0 }); save(); render(); });
            body.appendChild(add);

            card.appendChild(body);
            content.appendChild(card);
        });

        if (!state.utilities.length) {
            content.appendChild(el('div', { class: 'empty-note', text: 'No utilities yet — add one to shape how goals compete for the brain.' }));
        }
        const add = el('button', { class: 'add-block', html: ICON.plus + 'Add ' + TAB_LABELS.utilities });
        add.addEventListener('click', () => { state.utilities.push(P.blankUtility()); save(); render(); });
        content.appendChild(add);
    }

    // --- render missions ---------------------------------------------------

    function renderMissions() {
        const accent = ACCENTS.missions;
        state.missions.forEach((m, i) => {
            expanded.add('m' + i);
            const card = el('article', { class: 'card' });
            card.style.setProperty('--card-accent', accent);

            const idInput = el('input', { class: 'id-input', type: 'text', spellcheck: 'false', value: m.id, title: 'Mission id' });
            idInput.addEventListener('input', () => { m.id = idInput.value; updateCounts(); save(); });
            const nameInput = el('input', { class: 'name-input', type: 'text', spellcheck: 'false', value: m.name });
            nameInput.addEventListener('input', () => { m.name = nameInput.value; save(); });

            const chev = el('span', { class: 'chevron', html: ICON.chevron });
            const head = el('div', { class: 'card-header' }, [
                chev,
                el('div', { class: 'card-title' }, [idInput, nameInput]),
                el('div', { class: 'card-actions' }, [
                    iconButton('up', 'Move up', () => move('missions', i, -1)),
                    iconButton('down', 'Move down', () => move('missions', i, 1)),
                    iconButton('trash', 'Delete mission', () => remove('missions', i), 'danger'),
                ]),
            ]);
            head.addEventListener('click', (e) => {
                if (e.target.closest('.card-actions')) return;
                card.classList.toggle('collapsed');
                if (card.classList.contains('collapsed')) expanded.delete('m' + i);
                else expanded.add('m' + i);
            });
            card.appendChild(head);

            const body = el('div', { class: 'card-body' });
            const ids = el('textarea', { spellcheck: 'false', rows: Math.max(2, m.goal_ids.length + 1) });
            ids.value = m.goal_ids.join('\n');
            ids.addEventListener('input', () => {
                m.goal_ids = ids.value.split('\n').map((s) => s.trim()).filter(Boolean);
                save();
            });
            const field = el('div', { class: 'goal-ids' }, [
                el('div', { class: 'field-label', text: 'Goal ids (one per line)' }),
                ids,
                el('div', { class: 'field-help', text: 'goals this mission pursues' }),
            ]);
            body.appendChild(field);

            const note = el('input', { type: 'text', spellcheck: 'false', value: m.note, placeholder: 'scan, collect one food, return' });
            note.addEventListener('input', () => { m.note = note.value; save(); });
            body.appendChild(el('div', { class: 'field', style: 'margin-top:12px' }, [fieldLabel('Note'), note]));

            card.appendChild(body);
            content.appendChild(card);
        });

        const add = el('button', { class: 'add-block', html: ICON.plus + 'Add ' + TAB_LABELS.missions });
        add.addEventListener('click', () => { state.missions.push(P.blankMission()); save(); render(); });
        content.appendChild(add);
    }

    // --- structural ops ----------------------------------------------------

    function move(tab, i, dir) {
        const list = state[tab];
        const j = i + dir;
        if (j < 0 || j >= list.length) return;
        [list[i], list[j]] = [list[j], list[i]];
        save();
        render();
    }

    function remove(tab, i) {
        state[tab].splice(i, 1);
        save();
        render();
    }

    function redrawCurve(i) {
        const cards = content.querySelectorAll('.card');
        const card = cards[i];
        if (!card) return;
        const curveBox = card.querySelector('.curve');
        if (!curveBox) return;
        curveBox.innerHTML = '';
        curveBox.appendChild(curveSVG(state.utilities[i]));
    }

    // --- render loop -------------------------------------------------------

    function render() {
        content.innerHTML = '';
        if (!state.actions.length) {
            content.appendChild(el('div', { class: 'empty-note', text: 'No actions yet — every GOAP plan starts with actions.' }));
        }
        if (activeTab === 'actions') renderActions();
        else if (activeTab === 'goals') renderGoals();
        else if (activeTab === 'utilities') renderUtilities();
        else renderMissions();

        for (const tab of ['actions', 'goals', 'utilities', 'missions']) {
            const t = document.querySelector('.tab[data-tab="' + tab + '"]');
            t.classList.toggle('active', tab === activeTab);
        }

        const nameInput = $('#plan-name-input');
        if (document.activeElement !== nameInput) nameInput.value = state.name;

        updateCounts();
        updateBadges();
    }

    // --- export ------------------------------------------------------------

    function blob() {
        return P.serialize(state);
    }

    function toHex(bytes) {
        let s = '';
        for (let i = 0; i < bytes.length; i++) s += bytes[i].toString(16).padStart(2, '0');
        return s;
    }

    function toBase64(bytes) {
        let bin = '';
        const CHUNK = 0x8000;
        for (let i = 0; i < bytes.length; i += CHUNK) {
            bin += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
        }
        return btoa(bin);
    }

    function toCArray(bytes, name) {
        const lines = [];
        let line = '  ';
        for (let i = 0; i < bytes.length; i++) {
            line += '0x' + bytes[i].toString(16).padStart(2, '0') + ', ';
            if ((i + 1) % 12 === 0) { lines.push(line); line = '  '; }
        }
        if (line.trim()) lines.push(line);
        return '// fleece GOAP plan: ' + (name || 'untitled') + ' (' + bytes.length + ' bytes)\n' +
            'static const unsigned char goap_plan[] = {\n' +
            lines.join('\n') +
            '\n};\n' +
            '// load with: fleece_goap_deserialize(goap, goap_plan, sizeof(goap_plan));\n';
    }

    function download(filename, mime, data) {
        const a = document.createElement('a');
        a.href = URL.createObjectURL(new Blob([data], { type: mime }));
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        setTimeout(() => URL.revokeObjectURL(a.href), 3000);
        a.remove();
    }

    function copyText(text) {
        if (navigator.clipboard && navigator.clipboard.writeText) {
            return navigator.clipboard.writeText(text);
        }
        const ta = document.createElement('textarea');
        ta.value = text;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        ta.remove();
        return Promise.resolve();
    }

    function safeName() {
        return (state.name || 'plan').trim().replace(/[^\w.-]+/g, '_') || 'plan';
    }

    function exportMenu(e) {
        e.stopPropagation();
        const bytes = blob();
        const base = safeName();
        const actions = [
            { label: 'Download plan blob (.bin)', fn: () => download(base + '.bin', 'application/octet-stream', bytes) },
            { label: 'Download plan JSON (.json)', fn: () => download(base + '.json', 'application/json', P.toJSON(state)) },
            { label: 'Copy blob as hex', fn: () => copyText(toHex(bytes)).then(() => toast('Copied ' + bytes.length + ' bytes of hex', 'ok')) },
            { label: 'Copy blob as base64', fn: () => copyText(toBase64(bytes)).then(() => toast('Copied ' + bytes.length + ' bytes of base64', 'ok')) },
            { label: 'Copy as C array', fn: () => copyText(toCArray(bytes, state.name)).then(() => toast('Copied C array', 'ok')) },
            { label: 'Copy plan JSON', fn: () => copyText(P.toJSON(state)).then(() => toast('Copied plan JSON', 'ok')) },
        ];
        const menu = el('div', { style: 'position:fixed;z-index:120;' });
        const wrap = el('div', {
            class: 'export-menu',
            style: 'background:var(--panel-3);border:1px solid var(--border);border-radius:12px;box-shadow:0 18px 50px rgba(0,0,0,.5);overflow:hidden;min-width:230px;',
        });
        for (const a of actions) {
            const b = el('button', {
                class: 'export-item',
                style: 'display:block;width:100%;text-align:left;background:transparent;border:none;color:var(--text);font:inherit;font-size:13px;padding:10px 14px;cursor:pointer;',
            }, a.label);
            b.addEventListener('mouseenter', () => { b.style.background = 'rgba(94,234,212,0.1)'; });
            b.addEventListener('mouseleave', () => { b.style.background = 'transparent'; });
            b.addEventListener('click', () => { a.fn(); closeMenu(); });
            wrap.appendChild(b);
        }
        menu.appendChild(wrap);
        document.body.appendChild(menu);

        const rect = e.currentTarget.getBoundingClientRect();
        const menuRect = wrap.getBoundingClientRect();
        let x = Math.min(rect.right - menuRect.width, window.innerWidth - menuRect.width - 12);
        let y = rect.bottom + 8;
        if (y + menuRect.height > window.innerHeight) y = rect.top - menuRect.height - 8;
        wrap.style.position = 'fixed';
        wrap.style.left = Math.max(12, x) + 'px';
        wrap.style.top = Math.max(12, y) + 'px';
        wrap.style.opacity = '0';
        requestAnimationFrame(() => { wrap.style.opacity = '1'; });

        window.closeMenu = closeMenu;
        function closeMenu() {
            if (menu.parentNode) menu.remove();
            window.closeMenu = null;
            window.removeEventListener('click', closeMenu);
        }
        setTimeout(() => window.addEventListener('click', closeMenu), 0);
    }

    // --- import ------------------------------------------------------------

    function openModal() {
        $('#modal-overlay').classList.add('open');
        $('#modal-textarea').value = '';
        setImportKind('json');
        setTimeout(() => $('#modal-textarea').focus(), 30);
    }

    function closeModal() {
        $('#modal-overlay').classList.remove('open');
    }

    function setImportKind(kind) {
        importKind = kind;
        document.querySelectorAll('#import-seg button').forEach((b) => {
            b.classList.toggle('active', b.dataset.importKind === kind);
        });
        const hints = {
            json: 'Paste a plan JSON document exported earlier.',
            blob: 'Paste the plan blob as hex (or drop a .bin/.hex/.blob file).',
            blob64: 'Paste the plan blob as base64.',
        };
        $('#modal-hint').textContent = hints[kind];
    }

    function bytesFromHex(hexStr) {
        const s = hexStr.replace(/\s+/g, '');
        if (!/^[0-9a-fA-F]*$/.test(s)) throw new Error('invalid hex');
        const out = new Uint8Array(Math.floor(s.length / 2));
        for (let i = 0; i < out.length; i++) out[i] = parseInt(s.substr(i * 2, 2), 16);
        return out;
    }

    function bytesFromBase64(b64) {
        const bin = atob(b64.trim());
        const out = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
        return out;
    }

    function doImport() {
        const text = $('#modal-textarea').value;
        try {
            if (importKind === 'json') {
                state = P.fromJSON(text);
            } else if (importKind === 'blob') {
                state = P.parse(bytesFromHex(text));
            } else {
                state = P.parse(bytesFromBase64(text));
            }
            save();
            closeModal();
            render();
            toast('Plan imported', 'ok');
        } catch (err) {
            toast('Import failed: ' + err.message, 'err');
        }
    }

    function importFile(file) {
        const name = (file.name || '').toLowerCase();
        const readText = (cb) => {
            const r = new FileReader();
            r.onload = () => cb(String(r.result));
            r.readAsText(file);
        };
        const readBytes = (cb) => {
            const r = new FileReader();
            r.onload = () => cb(new Uint8Array(r.result));
            r.readAsArrayBuffer(file);
        };
        try {
            if (name.endsWith('.json')) {
                readText((t) => { state = P.fromJSON(t); finishFileImport(); });
            } else if (name.endsWith('.bin') || name.endsWith('.blob') || name.endsWith('.plan')) {
                readBytes((b) => { state = P.parse(b); finishFileImport(); });
            } else {
                readText((t) => {
                    const trimmed = t.trim();
                    if (trimmed.startsWith('{')) state = P.fromJSON(trimmed);
                    else if (/^[0-9a-fA-F\s]+$/.test(trimmed)) state = P.parse(bytesFromHex(trimmed));
                    else state = P.parse(bytesFromBase64(trimmed));
                    finishFileImport();
                });
            }
        } catch (err) {
            toast('Import failed: ' + err.message, 'err');
        }
        function finishFileImport() {
            save();
            closeModal();
            render();
            toast('Imported ' + file.name, 'ok');
        }
    }

    // --- toast -------------------------------------------------------------

    function toast(text, kind) {
        const t = $('#toast');
        const span = $('#toast-text');
        t.className = 'toast show ' + (kind || 'ok');
        span.textContent = text;
        clearTimeout(toastTimer);
        toastTimer = setTimeout(() => { t.className = 'toast'; }, 2600);
    }

    // --- wiring ------------------------------------------------------------

    function wire() {
        $('#btn-export').addEventListener('click', exportMenu);

        $('#btn-import').addEventListener('click', openModal);
        $('#modal-close').addEventListener('click', closeModal);
        $('#modal-cancel').addEventListener('click', closeModal);
        $('#modal-overlay').addEventListener('click', (e) => { if (e.target.id === 'modal-overlay') closeModal(); });
        $('#modal-ok').addEventListener('click', doImport);
        document.querySelectorAll('#import-seg button').forEach((b) => {
            b.addEventListener('click', () => setImportKind(b.dataset.importKind));
        });

        const fileInput = el('input', { type: 'file', accept: '.json,.bin,.blob,.plan,.hex,.txt', style: 'display:none' });
        fileInput.addEventListener('change', () => {
            if (fileInput.files && fileInput.files[0]) importFile(fileInput.files[0]);
            fileInput.value = '';
        });
        document.body.appendChild(fileInput);
        const fileBtn = el('button', { class: 'btn ghost', style: 'margin-right:auto', text: 'Load file…' });
        fileBtn.addEventListener('click', () => fileInput.click());
        $('.modal-footer').insertBefore(fileBtn, $('.modal-footer').firstChild);

        $('#btn-sample').addEventListener('click', () => {
            state = example4Model();
            expanded = new Set();
            save();
            render();
            toast('Loaded the example4 forager scenario', 'ok');
        });

        $('#plan-name-input').addEventListener('input', (e) => {
            state.name = e.target.value;
            save();
            updateBadges();
        });

        document.querySelectorAll('.tab').forEach((t) => {
            t.addEventListener('click', () => { activeTab = t.dataset.tab; render(); });
        });

        window.addEventListener('keydown', (e) => {
            if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') {
                e.preventDefault();
                download(safeName() + '.bin', 'application/octet-stream', blob());
                toast('Exported ' + blob().length + '-byte plan blob', 'ok');
            }
        });
    }

    // --- boot --------------------------------------------------------------

    function boot() {
        if (!load()) {
            state = example4Model();
        }
        expanded = new Set();
        wire();
        render();
    }

    if (typeof document !== 'undefined') {
        boot();
    }

    // Node self-test hook (harmless in the browser).
    if (typeof module !== 'undefined' && module.exports) {
        module.exports = {
            FleecePlan: P,
            example4Model,
            getState: () => state,
            setState: (m) => { state = m; },
            switchTab: (t) => { activeTab = t; render(); },
        };
    }
})();