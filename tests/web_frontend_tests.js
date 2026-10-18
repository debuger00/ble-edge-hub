// Exercise the actual app.js event handlers with lightweight DOM/Chart doubles.
const assert = require('assert').strict;
const fs = require('fs');
const path = require('path');
const vm = require('vm');

function element() {
    return {
        children: [], dataset: {}, style: {}, classList: { add() {} },
        set innerHTML(value) { this.children = []; },
        getContext() { return {}; },
        appendChild(child) { this.children.push(child); },
        querySelectorAll() { return this.children; },
        querySelector() { return element(); }
    };
}
const elements = new Map();
const intervals = [];
const timeouts = [];
const charts = [];
const sockets = [];
const document = {
    hidden: false,
    getElementById(id) {
        if (id.startsWith('download-')) return null;
        if (!elements.has(id)) elements.set(id, element());
        return elements.get(id);
    },
    createElement: element,
    querySelector: element
};
class Chart {
    constructor(_, config) {
        Object.assign(this, config);
        this.updates = 0;
        charts.push(this);
    }
    update() { this.updates++; }
}
class WebSocket {
    static OPEN = 1;
    constructor(url) {
        this.url = url;
        this.readyState = 1;
        this.sent = [];
        sockets.push(this);
    }
    send(payload) { this.sent.push(JSON.parse(payload)); }
}
const storage = new Map();
const localStorage = {
    getItem(key) { return storage.get(key) || null; },
    setItem(key, value) { storage.set(key, value); }
};
const context = vm.createContext({
    document, Chart, WebSocket, console, URLSearchParams,
    window: { localStorage, location: { hostname: 'edge.local', search: '?wsPort=9000', protocol: 'http:' } },
    setInterval(fn, ms) { intervals.push({ fn, ms }); },
    setTimeout(fn) { timeouts.push(fn); return timeouts.length; },
    clearTimeout() {}
});
vm.runInContext(fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8'), context);
const render = intervals.find(timer => timer.ms < 100).fn;
const a = 'AA:BB:CC:DD:EE:01', b = 'AA:BB:CC:DD:EE:02';
const send = message => sockets[sockets.length - 1].onmessage({ data: JSON.stringify(message) });
const deviceList = { type: 'device_list', devices: [{ id: '001', type: 'imu948', mac: a }, { id: '005', type: 'md9930', mac: b }] };
const sample = (mac, i) => ({ type: 'sensor_data', mac, timestamp: 100000 + i * 40,
    sensor_uptime: i * 40, data: { accel: { x: i, y: 2, z: 3 } } });
sockets[0].onopen();
assert.equal(sockets[0].url, 'ws://edge.local:9000');
send(deviceList);
assert.deepEqual(sockets[0].sent[sockets[0].sent.length - 1], { type: 'subscribe', mac: a, sample_hz: 0 });
assert.equal(elements.get('current-device-mac').textContent, '001 · imu948 · ' + a);
render();
const updates = charts.map(chart => chart.updates);
for (let i = 0; i < 25; ++i) {
    send(sample(a, i));
    send(sample(b, i));
}
assert.deepEqual(charts.map(chart => chart.updates), updates, 'receiving must not render');
assert.equal(charts[0].data.datasets[0].data.length, 25, 'preserve full-rate samples');
assert.equal(charts.length, 2, 'only acceleration and action charts remain');
render();
assert.equal(charts[0].updates, updates[0] + 1);
render();
assert.equal(charts[0].updates, updates[0] + 1, 'clean charts must not redraw');

elements.get('device-selector').children[1].onclick();
assert.deepEqual(sockets[0].sent[sockets[0].sent.length - 1], { type: 'subscribe', mac: b, sample_hz: 0 });
assert.equal(charts[0].data.datasets[0].data.length, 0);
send(sample(a, 26));
assert.equal(charts[0].data.datasets[0].data.length, 0, 'ignore old-device in-flight messages');
send(sample(b, 26));
const rate = elements.get('display-rate');
rate.value = '10';
rate.onchange();
assert.deepEqual(sockets[0].sent[sockets[0].sent.length - 1], { type: 'subscribe', mac: b, sample_hz: 10 });

document.hidden = true;
const beforeHidden = charts[0].updates;
for (let i = 27; i < 1200; ++i) send(sample(b, i));
render();
assert.equal(charts[0].updates, beforeHidden);
assert.ok(charts[0].data.datasets[0].data.length <= 750, 'hidden-tab memory is bounded');
document.hidden = false;
render();
assert.equal(charts[0].updates, beforeHidden + 1);

send({ type: 'device_activity', mac: a, age_ms: 1000 });
assert.ok(vm.runInContext(`Date.now() - deviceLastSeen.get('${a}') < 1500`, context));
sockets[0].onclose({});
assert.equal(vm.runInContext('deviceLastSeen.size', context), 0);
timeouts[timeouts.length - 1]();
sockets[1].onopen();
send(deviceList);
assert.deepEqual(sockets[1].sent[sockets[1].sent.length - 1], { type: 'subscribe', mac: b, sample_hz: 10 });
assert.equal(charts[0].data.datasets[0].data.length, 0, 'reconnect clears stale chart history');
send({ type: 'device_list', devices: [] });
assert.deepEqual(sockets[1].sent[sockets[1].sent.length - 1], { type: 'subscribe', mac: '', sample_hz: 10 });
send({ type: 'battery', mac: a, level: 72, timestamp: 1700000000000 });
send({ type: 'battery', mac: a, level: 15, timestamp: 1699999999000 });
send({ type: 'battery', mac: a, level: 999, timestamp: 1700000001000 });
assert.equal(JSON.parse(storage.get('ble-iot-last-battery-v1'))[0][1].level, 72,
    'older and invalid readings must not replace latest battery');
console.log('Frontend tests passed: subscription, batching, bounded data, status, reconnect.');

// Old cached HTML lacks the new controls; initialization and subscription must
// still work instead of failing before opening the WebSocket.
const legacyDocument = Object.assign({}, document, {
    getElementById(id) {
        if (id === 'display-rate' || id === 'waveform-status') return null;
        return document.getElementById(id);
    }
});
const legacyContext = vm.createContext({
    document: legacyDocument, Chart, WebSocket, console, URLSearchParams,
    window: { localStorage, location: { hostname: 'edge.local', host: 'edge.local:8080', search: '', protocol: 'http:' } },
    setInterval() {}, setTimeout() {}, clearTimeout() {}
});
vm.runInContext(fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8'), legacyContext);
const legacySocket = sockets[sockets.length - 1];
assert.equal(legacySocket.url, 'ws://edge.local:8080/ws');
legacySocket.onopen();
legacySocket.onmessage({ data: JSON.stringify(deviceList) });
assert.deepEqual(legacySocket.sent[0], { type: 'subscribe', mac: a, sample_hz: 0 });
console.log('Cached HTML compatibility passed.');

assert.equal(vm.runInContext(`deviceBatteryMap.get('${a}').level`, legacyContext), 72);
assert.equal(vm.runInContext(`deviceBatteryMap.get('${a}').timestamp`, legacyContext), 1700000000000,
    'page reload restores original battery measurement time');
console.log('Battery persistence and stale reading checks passed.');
