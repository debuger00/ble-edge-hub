// web/app.js

// =======================
// 1. 常量定义与状态管理 (State Management)
// =======================

const MAX_DATA_POINTS = 750;
const SAMPLE_WINDOW_MS = 30000;
const RENDER_INTERVAL_MS = 1000 / 15;
const dirtyCharts = new Set();
let displaySampleHz = 0; // 0: retain every received sample; 10: display-only downsampling.
const ACTION_WINDOW_MS = 20000;           // action 时间窗口 (20秒)

const discoveredDevices = new Set();
let currentDeviceMac = null;
let selectedSampleCount = 0;
let selectedLastSampleAt = 0;
let confirmedSubscriptionMac = null;

const OFFLINE_THRESHOLD_MS = 5000; // 判定离线的阈值：5000ms未收到数据
const deviceLastSeen = new Map();  // 记录设备最后活跃的绝对时间 (Space Complexity: O(N))
const deviceBatteryMap = new Map(); // 缓存电量与时间戳数据 (Space Complexity: O(N))

// DOM 引用
const statusElement = document.getElementById('status');
const deviceSelectorElement = document.getElementById('device-selector');
const currentDeviceMacElement = document.getElementById('current-device-mac');
const displayRateElement = document.getElementById('display-rate');
const waveformStatusElement = document.getElementById('waveform-status');

// =======================
// 2. 图表初始化 (Chart.js Initialization)
// =======================

const accelChart = document.getElementById('accelChart').getContext('2d');
const chart = new Chart(accelChart, {
    type: 'line',
    data: {
        datasets: [
            { label: 'Accel X', borderColor: 'rgb(255, 99, 132)', data: [], tension: 0.1 },
            { label: 'Accel Y', borderColor: 'rgb(54, 162, 235)', data: [], tension: 0.1 },
            { label: 'Accel Z', borderColor: 'rgb(75, 192, 192)', data: [], tension: 0.1 }
        ]
    },
    options: {
        scales: {
            x: { type: 'time', time: { unit: 'second' } },
            y: { title: { display: true, text: 'm/s²' } }
        },
        animation: false
    }
});

const actionCtx = document.getElementById('actionChart').getContext('2d');
const actionLabels = ["standing", "running", "grazing", "trotting", "walking"];

const actionChart = new Chart(actionCtx, {
    type: 'bar',
    data: {
        datasets: [{
            label: '识别出的行为',
            data: [],
            backgroundColor: (ctx) => {
                if (ctx.raw && ctx.raw.y) {
                    const action = ctx.raw.y;
                    const colors = {
                        standing: 'rgba(158, 158, 158, 0.7)',
                        walking: 'rgba(76, 175, 80, 0.7)',
                        running: 'rgba(244, 67, 54, 0.7)',
                        grazing: 'rgba(33, 150, 243, 0.7)',
                        trotting: 'rgba(255, 152, 0, 0.7)',
                    };
                    return colors[action] || 'rgba(100, 100, 100, 0.7)';
                }
                return 'rgba(204, 204, 204, 0.7)';
            },
            barPercentage: 1.0,
            categoryPercentage: 1.0,
            borderWidth: 1,
            borderColor: 'rgba(0, 0, 0, 0.1)'
        }]
    },
    options: {
        indexAxis: 'y',
        animation: false,
        plugins: {
            legend: { display: false },
            tooltip: {
                enabled: true,
                callbacks: {
                    label: function (context) {
                        const start = new Date(context.parsed.x[0]).toLocaleTimeString();
                        const end = new Date(context.parsed.x[1]).toLocaleTimeString();
                        return `${context.raw.y}: from ${start} to ${end}`;
                    }
                }
            }
        },
        scales: {
            x: {
                type: 'time',
                position: 'bottom',
                time: { unit: 'second', displayFormats: { second: 'HH:mm:ss' } }
            },
            y: {
                type: 'category',
                labels: actionLabels,
                offset: true,
                ticks: { autoSkip: false }
            }
        }
    }
});

const sensorTimeCtx = document.getElementById('sensorTimeChart').getContext('2d');
const sensorTimeChart = new Chart(sensorTimeCtx, {
    type: 'line',
    data: {
        datasets: [
            { label: 'Accel X (Device Time)', borderColor: 'rgb(75, 192, 192)', data: [], tension: 0.1, pointRadius: 2 },
            { label: 'Accel Y (Device Time)', borderColor: 'rgb(255, 99, 132)', data: [], tension: 0.1, pointRadius: 2 },
            { label: 'Accel Z (Device Time)', borderColor: 'rgb(54, 162, 235)', data: [], tension: 0.1, pointRadius: 2 }
        ]
    },
    options: {
        scales: {
            x: {
                type: 'linear',
                title: { display: true, text: 'Sensor Uptime (ms)' },
                ticks: { callback: function (value) { return value; } }
            },
            y: { title: { display: true, text: 'm/s²' } }
        },
        animation: false,
        plugins: {
            tooltip: {
                callbacks: { title: (context) => `Uptime: ${context[0].parsed.x} ms` }
            }
        }
    }
});

// =======================
// 3. UI 渲染与状态轮询 (UI Rendering & Polling)
// =======================

/**
 * 重新渲染选择按钮，将电量和时间节点静态挂载到 DOM 中
 */
function renderDeviceSelector() {
    deviceSelectorElement.innerHTML = '';

    discoveredDevices.forEach(mac => {
        const button = document.createElement('button');
        button.className = 'button';
        button.dataset.mac = mac;

        // 从缓存中提取电量与时间上下文
        const batteryData = deviceBatteryMap.get(mac);
        const battDisplay = batteryData ? 'flex' : 'none';
        const battLevel = batteryData ? batteryData.level : '--';
        const battTime = batteryData ? new Date(batteryData.timestamp).toLocaleTimeString() : '--:--:--';

        // 构造双行布局的按钮内部 DOM (第一行：MAC + 在线状态；第二行：电量 + 时间)
        button.innerHTML = `
            <div class="is-flex is-align-items-center w-100">
                <span class="has-text-weight-bold">${mac}</span> 
                <span class="tag status-tag is-light is-rounded ml-2">Offline</span>
            </div>
            <div class="battery-container is-flex is-align-items-center mt-1" style="display: ${battDisplay}; font-size: 0.75rem;">
                <span class="tag is-info is-light is-small">
                    🔋 <strong class="batt-level ml-1">${battLevel}%</strong>
                </span>
                <span class="has-text-grey ml-2">
                    ⏱️ <span class="batt-time">${battTime}</span>
                </span>
            </div>
        `;

        if (mac === currentDeviceMac) {
            button.classList.add('is-primary');
        }

        button.onclick = () => {
            if (currentDeviceMac !== mac) {
                currentDeviceMac = mac;
                currentDeviceMacElement.textContent = mac;

                resetCharts();
                subscribeCurrentDevice();

                renderDeviceSelector();
                updateDeviceStatusUI();
            }
        };
        deviceSelectorElement.appendChild(button);
    });
}

function resetCharts() {
    selectedSampleCount = 0;
    selectedLastSampleAt = 0;
    confirmedSubscriptionMac = null;
    [chart, sensorTimeChart, actionChart].forEach(item => {
        item.data.datasets.forEach(dataset => { dataset.data = []; });
        delete item.options.scales.x.min;
        delete item.options.scales.x.max;
        dirtyCharts.add(item);
    });
}

// Network handlers only update bounded datasets. Rendering is independent of
// input frequency, and hidden tabs keep at most MAX_DATA_POINTS per dataset.
setInterval(() => {
    if (document.hidden) return;
    dirtyCharts.forEach(item => item.update('none'));
    dirtyCharts.clear();
}, RENDER_INTERVAL_MS);

function subscribeCurrentDevice() {
    if (socket && socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({
            type: 'subscribe', mac: currentDeviceMac || '', sample_hz: displaySampleHz
        }));
    }
}

// A cached older HTML page may not contain the new optional display control.
// It must still initialize the socket and subscribe successfully.
if (displayRateElement) {
    displayRateElement.onchange = () => {
        displaySampleHz = Number(displayRateElement.value) === 10 ? 10 : 0;
        subscribeCurrentDevice();
    };
}

function updateWaveformStatus() {
    if (!waveformStatusElement) return;
    if (!socket || socket.readyState !== WebSocket.OPEN) {
        waveformStatusElement.textContent = '实时连接未建立，暂时无法接收波形。';
    } else if (!currentDeviceMac) {
        waveformStatusElement.textContent = '没有可订阅的设备，请检查后端设备配置。';
    } else if (selectedLastSampleAt && Date.now() - selectedLastSampleAt <= OFFLINE_THRESHOLD_MS) {
        waveformStatusElement.textContent = `正在接收 ${currentDeviceMac}：本次已收到 ${selectedSampleCount} 个波形采样。`;
    } else if (confirmedSubscriptionMac === currentDeviceMac) {
        waveformStatusElement.textContent = `已订阅 ${currentDeviceMac}，等待采样；可选择 Online 设备或检查传感器连接。`;
    } else {
        waveformStatusElement.textContent = `等待 ${currentDeviceMac} 的订阅确认；若持续无数据，请确认后端已更新。`;
    }
}

currentDeviceMacElement.textContent = currentDeviceMac || "--";
renderDeviceSelector();

/**
 * 看门狗：基于时间戳偏差验证在线状态
 * Time Complexity: O(N)
 */
function updateDeviceStatusUI() {
    const currentNow = Date.now();
    const buttons = deviceSelectorElement.querySelectorAll('button[data-mac]');

    buttons.forEach(button => {
        const mac = button.dataset.mac;
        const lastSeen = deviceLastSeen.get(mac) || 0;
        const isOffline = (currentNow - lastSeen) > OFFLINE_THRESHOLD_MS;

        const tag = button.querySelector('.status-tag');
        if (!tag) return;

        if (isOffline) {
            button.style.opacity = '0.6';
            tag.className = 'tag status-tag is-light is-rounded ml-2';
            tag.textContent = 'Offline';
        } else {
            button.style.opacity = '1.0';
            tag.className = 'tag status-tag is-success is-rounded ml-2';
            tag.textContent = 'Online';
        }
    });
}

setInterval(() => {
    updateDeviceStatusUI();
    updateWaveformStatus();
}, 1000);

// =======================
// 4. WebSocket 数据管道 (WebSocket Data Pipeline)
// =======================

let socket = null;
let reconnectInterval = 1000;
const maxReconnectInterval = 30000;
let reconnectTimer = null;

function connectWebSocket() {
    const wsHost = window.location.hostname;
    const wsPort = new URLSearchParams(window.location.search).get('wsPort') || 8765;
    const wsScheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const wsUrl = `${wsScheme}://${wsHost}:${wsPort}`;

    statusElement.textContent = '正在连接服务器...';
    statusElement.className = 'subtitle is-warning';

    socket = new WebSocket(wsUrl);

    socket.onopen = () => {
        statusElement.textContent = '已连接到服务器';
        statusElement.className = 'subtitle is-success';

        reconnectInterval = 1000;
        if (reconnectTimer) {
            clearTimeout(reconnectTimer);
            reconnectTimer = null;
        }
    };

    socket.onmessage = (event) => {
        try {
            const message = JSON.parse(event.data);

            // 设备列表初始化消息
            if (message.type === 'device_list') {
                discoveredDevices.clear();
                (message.devices || []).forEach(d => discoveredDevices.add(d.mac));
                if (!discoveredDevices.has(currentDeviceMac)) {
                    currentDeviceMac = discoveredDevices.values().next().value || null;
                }
                currentDeviceMacElement.textContent = currentDeviceMac || '--';
                resetCharts();
                renderDeviceSelector();
                subscribeCurrentDevice(); // Also restores the selection after reconnect.
                return;
            }
            if (message.type === 'subscribed') {
                confirmedSubscriptionMac = message.mac;
                updateWaveformStatus();
                return;
            }
            if (message.type === 'error') {
                statusElement.textContent = message.message || '订阅失败';
                statusElement.className = 'subtitle is-warning';
                return;
            }

            const mac = message.mac;
            const now = message.timestamp;

            if (message.type === 'device_activity') {
                if (mac && Number.isFinite(message.age_ms)) {
                    deviceLastSeen.set(mac, Date.now() - Math.max(0, message.age_ms));
                }
                return;
            }
            if (mac && message.type === 'sensor_data') {
                deviceLastSeen.set(mac, Date.now());
            }

            // --- 核心逻辑：电量消息分支提取与局部 O(1) DOM 更新 ---
            if (message.type === 'battery') {
                deviceBatteryMap.set(mac, { level: message.level, timestamp: now });

                // 定位特定的 DOM 节点更新文本，避免触发页面重排 (Reflow)
                const btn = document.querySelector(`button[data-mac="${mac}"]`);
                if (btn) {
                    const battContainer = btn.querySelector('.battery-container');
                    const battLevelSpan = btn.querySelector('.batt-level');
                    const battTimeSpan = btn.querySelector('.batt-time');

                    if (battContainer && battLevelSpan && battTimeSpan) {
                        battContainer.style.display = 'flex';
                        battLevelSpan.textContent = `${message.level}%`;
                        battTimeSpan.textContent = new Date(now).toLocaleTimeString();
                    }
                }
                return; // Early return，终止执行后续高频渲染逻辑
            }

            // 设备发现映射
            if (mac && !discoveredDevices.has(mac)) {
                discoveredDevices.add(mac);
                renderDeviceSelector();
                if (!currentDeviceMac) {
                    currentDeviceMac = mac;
                    currentDeviceMacElement.textContent = mac;
                    resetCharts();
                    subscribeCurrentDevice();
                    renderDeviceSelector();
                }
            }

            // 过滤异设备数据流
            if (mac !== currentDeviceMac) return;

            // 角速度与加速度渲染逻辑
            if (message.data && message.data.accel) {
                selectedSampleCount++;
                selectedLastSampleAt = Date.now();
                const accel = message.data.accel;
                const dx = chart.data.datasets[0].data;
                const dy = chart.data.datasets[1].data;
                const dz = chart.data.datasets[2].data;

                dx.push({ x: now, y: accel.x });
                dy.push({ x: now, y: accel.y });
                dz.push({ x: now, y: accel.z });

                if (dx.length > MAX_DATA_POINTS) dx.shift();
                if (dy.length > MAX_DATA_POINTS) dy.shift();
                if (dz.length > MAX_DATA_POINTS) dz.shift();

                chart.data.datasets.forEach(dataset => {
                    while (dataset.data.length && dataset.data[0].x < now - SAMPLE_WINDOW_MS) {
                        dataset.data.shift();
                    }
                });
                chart.options.scales.x.min = now - SAMPLE_WINDOW_MS;
                chart.options.scales.x.max = now;
                dirtyCharts.add(chart);

                const sensorTime = message.sensor_uptime;
                if (Number.isFinite(sensorTime)) {
                    const previous = sensorTimeChart.data.datasets[0].data;
                    if (previous.length && sensorTime < previous[previous.length - 1].x) {
                        sensorTimeChart.data.datasets.forEach(dataset => { dataset.data = []; });
                    }
                    const d1 = sensorTimeChart.data.datasets[0].data;
                    const d2 = sensorTimeChart.data.datasets[1].data;
                    const d3 = sensorTimeChart.data.datasets[2].data;

                    d1.push({ x: sensorTime, y: accel.x });
                    d2.push({ x: sensorTime, y: accel.y });
                    d3.push({ x: sensorTime, y: accel.z });

                    if (d1.length > MAX_DATA_POINTS) d1.shift();
                    if (d2.length > MAX_DATA_POINTS) d2.shift();
                    if (d3.length > MAX_DATA_POINTS) d3.shift();

                    sensorTimeChart.data.datasets.forEach(dataset => {
                        while (dataset.data.length && dataset.data[0].x < sensorTime - SAMPLE_WINDOW_MS) {
                            dataset.data.shift();
                        }
                    });
                    sensorTimeChart.options.scales.x.min = Math.max(0, sensorTime - SAMPLE_WINDOW_MS);
                    sensorTimeChart.options.scales.x.max = Math.max(1, sensorTime);
                    dirtyCharts.add(sensorTimeChart);
                }
            }

            // 行为推断时序渲染逻辑
            if (message.type === 'prediction') {
                const action = message.action;
                if (!actionLabels.includes(action)) return;

                const dataset = actionChart.data.datasets[0];
                const data = dataset.data;
                const lastBar = data.length > 0 ? data[data.length - 1] : null;

                if (lastBar && lastBar.y === action) {
                    lastBar.x[1] = now;
                } else {
                    if (lastBar) lastBar.x[1] = now;
                    data.push({ x: [now, now], y: action });
                }

                const minTime = now - ACTION_WINDOW_MS;
                actionChart.options.scales.x.min = minTime;
                actionChart.options.scales.x.max = now;

                while (data.length > 0 && data[0].x[1] < minTime) {
                    data.shift();
                }

                if (data.length > MAX_DATA_POINTS) data.splice(0, data.length - MAX_DATA_POINTS);
                dirtyCharts.add(actionChart);
            }

        } catch (e) {
            console.error('Payload parse failed:', e);
        }
    };

    socket.onclose = (event) => {
        deviceLastSeen.clear();
        confirmedSubscriptionMac = null;
        updateDeviceStatusUI();
        updateWaveformStatus();
        statusElement.textContent = `与服务器断开连接，将在 ${reconnectInterval / 1000} 秒后尝试重连...`;
        statusElement.className = 'subtitle is-danger';

        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(connectWebSocket, reconnectInterval);
        reconnectInterval = Math.min(reconnectInterval * 2, maxReconnectInterval); // 指数退避重连机制
    };

    socket.onerror = () => {
        statusElement.textContent = '连接错误，请检查网络堆栈。';
        statusElement.className = 'subtitle is-danger';
    };
}

connectWebSocket();
