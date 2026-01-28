// tr-web frontend JavaScript

// Reverse proxy support: detect subpath (e.g., /tr-web/) and adjust API/SSE URLs
const BASE_PATH = (() => {
    const path = window.location.pathname;
    if (path === '/' || path === '' || path.endsWith('index.html')) {
        return '';
    }
    const lastSlash = path.lastIndexOf('/');
    return lastSlash > 0 ? path.substring(0, lastSlash + 1) : '';
})();

// State management
const state = {
    connected: false,
    recorders: [],
    calls: [],
    stoppedCalls: new Map(),
    recentCalls: [],
    recentRetention: 20,
    selectedRecentCallKey: null,
    selectedActiveCallKey: null,
    lastActiveByKey: new Map(),
    lastActiveSeenAt: new Map(),
    endedKeyTimes: new Map(),
    missingSince: new Map(),
    devices: [],
    systems: [],
    rates: {},
    rateHistory: {},  // Per-system rate history
    callRateHistory: {},  // Per-system call count history
    config: null,
    consoleLogs: [],
    consoleMaxLines: 5000,  // Default, overridden by backend config
    trunkMessages: [],  // Omnitrunker: control channel messages
    unitAffiliations: {},  // Omnitrunker: unit -> talkgroup map
    omniSystemFilter: '',
    omniMsgTypeFilter: '',
    omniAutoScroll: true,
    omniBufferSize: 200  // Default buffer size for unit activity
};

const CALL_CACHE_MAX_AGE_MS = 10 * 60 * 1000;
const MISSING_END_GRACE_MS = 10000;

// Track console scroll state
let hasScrolledConsole = false;

// Utility functions
function escapeHtml(s) {
    return String(s)
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
}

function markEndedKey(key) {
    if (!key) return;
    state.endedKeyTimes.set(key, Date.now());
}

function hasEndedRecently(key) {
    if (!key) return false;
    const t = state.endedKeyTimes.get(key);
    return (t !== undefined) && ((Date.now() - t) <= CALL_CACHE_MAX_AGE_MS);
}

function pruneCallCaches() {
    const now = Date.now();

    for (const [k, t] of state.endedKeyTimes.entries()) {
        if ((now - t) > CALL_CACHE_MAX_AGE_MS) state.endedKeyTimes.delete(k);
    }
    for (const [k, t] of state.lastActiveSeenAt.entries()) {
        if ((now - t) > CALL_CACHE_MAX_AGE_MS) {
            state.lastActiveSeenAt.delete(k);
            state.lastActiveByKey.delete(k);
        }
    }

    for (const [k, t] of state.missingSince.entries()) {
        if ((now - t) > CALL_CACHE_MAX_AGE_MS) state.missingSince.delete(k);
    }

    // Safety: cap cache sizes even if timestamps are missing.
    const capMap = (m, max) => {
        if (m.size <= max) return;
        const keys = Array.from(m.keys());
        for (let i = 0; i < keys.length - max; i++) m.delete(keys[i]);
    };
    capMap(state.endedKeyTimes, 5000);
    capMap(state.lastActiveByKey, 5000);
    capMap(state.lastActiveSeenAt, 5000);
    capMap(state.missingSince, 5000);
}

// Authentication utilities  
async function getCurrentAuthLevel() {
    try {
        const response = await fetch(`${BASE_PATH}/api/whoami`, {
            credentials: 'include',
            cache: 'no-store'
        });
        
        if (response.ok) {
            const data = await response.json();
            if (data.auth_level === 'admin') {
                return { level: 'admin', label: 'Administrator' };
            } else if (data.auth_level === 'info') {
                return { level: 'info', label: 'Read-Only' };
            }
        }
    } catch (err) {
        console.error('Error detecting auth level:', err);
    }
    return { level: 'none', label: 'Unauthenticated' };
}

async function detectAuthLevelFromAdminAccess() {
    try {
        const response = await fetch(`${BASE_PATH}/api/admin/login-history`, {
            credentials: 'include',
            cache: 'no-store'
        });
        
        if (response.ok) {
            return 'admin';
        }
    } catch (err) {
        // Silently fail
    }
    return 'info';
}

function logout() {
    // Clear credentials by sending wrong credentials to trigger 401
    fetch(`${BASE_PATH}/api/health`, {
        headers: {
            'Authorization': 'Basic ' + btoa('logout:logout')
        }
    }).then(() => {
        // Reload page to trigger browser auth prompt
        window.location.reload();
    }).catch(() => {
        // Even if fetch fails, reload to clear state
        window.location.reload();
    });
}

async function updateAuthDisplay() {
    const authInfo = await getCurrentAuthLevel();
    const authLevelEl = document.getElementById('authLevel');
    const logoutBtn = document.getElementById('logoutBtn');
    
    if (authInfo.level !== 'none') {
        authLevelEl.innerHTML = `<span class="badge badge-${authInfo.level}">${authInfo.label}</span>`;
        authLevelEl.style.display = 'inline';
        logoutBtn.style.display = 'inline-block';
    }
}

function cssVar(name, fallback) {
    const v = getComputedStyle(document.documentElement).getPropertyValue(name);
    const t = (v || '').trim();
    return t || fallback;
}

function getChartPalette() {
    // Use theme primitives (no hard-coded new colors)
    return [
        cssVar('--accent-blue', '#3498db'),
        cssVar('--accent-green', '#00d26a'),
        cssVar('--accent-yellow', '#ffc107'),
        cssVar('--accent', '#e94560'),
        cssVar('--accent-cyan', '#1abc9c'),
        cssVar('--accent-purple', '#9b59b6'),
        cssVar('--accent-orange', '#e67e22'),
        cssVar('--accent-pink', '#e94560')
    ];
}

// SSE connection
let eventSource = null;
let reconnectTimeout = null;
let lastDisconnectedAt = 0;

// Themes
const THEMES = [
    { id: 'nostromo', name: 'Nostromo' },
    { id: 'classic', name: 'Classic' },
    { id: 'hotdog', name: 'Hot Dog Stand' }
];

function applyTheme(themeId) {
    const chosen = (THEMES.find(t => t.id === themeId) ? themeId : 'nostromo');
    document.documentElement.dataset.theme = chosen;
    const sel = document.getElementById('themeSelect');
    if (sel) sel.value = chosen;
    
    // Redraw charts immediately to apply new theme colors
    updateChart();
    updateCallRateChart();
}

function initThemePicker() {
    const sel = document.getElementById('themeSelect');
    if (!sel) return;

    sel.innerHTML = THEMES.map(t => `<option value="${t.id}">${t.name}</option>`).join('');
    sel.addEventListener('change', () => applyTheme(sel.value));
}

function connect() {
    if (eventSource) {
        eventSource.close();
    }
    
    eventSource = new EventSource(`${BASE_PATH}events`);
    
    eventSource.onopen = () => {
        state.connected = true;
        updateConnectionStatus();
        console.log('SSE connected');

        // If we were disconnected for a while (service restart, network blip),
        // drop short-lived call caches so we don't suppress synthetic ends.
        if (lastDisconnectedAt && (Date.now() - lastDisconnectedAt) > 1500) {
            state.stoppedCalls.clear();
            state.endedKeyTimes.clear();
            state.missingSince.clear();
            state.selectedActiveCallKey = null;
        }
        lastDisconnectedAt = 0;
    };
    
    eventSource.onerror = () => {
        state.connected = false;
        updateConnectionStatus();
        eventSource.close();

        if (!lastDisconnectedAt) lastDisconnectedAt = Date.now();
        
        // Reconnect after 3 seconds
        if (reconnectTimeout) clearTimeout(reconnectTimeout);
        reconnectTimeout = setTimeout(connect, 3000);
    };
    
    eventSource.addEventListener('recorders', (e) => {
        const data = JSON.parse(e.data);
        state.recorders = data.recorders || [];
        updateRecordersTable();
        updateStats();
    });
    
    eventSource.addEventListener('calls', (e) => {
        const data = JSON.parse(e.data);
        const nextCalls = data.calls_active || [];

        const nowMs = Date.now();

        // Detect calls that disappeared (encrypted calls may not send call_end)
        const prevKeys = new Set(state.calls.map(callKey).filter(Boolean));
        const nextKeys = new Set(nextCalls.map(callKey).filter(Boolean));

        // Update last-seen cache from the incoming set before we process removals
        for (const c of nextCalls) {
            const k = callKey(c);
            if (k) {
                state.lastActiveByKey.set(k, c);
                state.lastActiveSeenAt.set(k, nowMs);
                state.missingSince.delete(k);
            }
        }

        // Track missing keys (but give call_end a grace window to arrive)
        for (const k of prevKeys) {
            if (!nextKeys.has(k) && !hasEndedRecently(k)) {
                if (!state.missingSince.has(k)) state.missingSince.set(k, nowMs);
            }
        }

        // After grace period, synthesize an end for any call that vanished without a call_end.
        // This fixes encrypted calls (and any other rare missing call_end scenarios).
        for (const [k, since] of state.missingSince.entries()) {
            if (nextKeys.has(k)) {
                state.missingSince.delete(k);
                continue;
            }
            if (hasEndedRecently(k)) {
                state.missingSince.delete(k);
                continue;
            }
            if ((nowMs - since) < MISSING_END_GRACE_MS) continue;

            const last = state.lastActiveByKey.get(k);
            if (last) {
                const nowSec = Math.floor(nowMs / 1000);
                const start = (last.start_time !== undefined && last.start_time !== null) ? Number(last.start_time) : null;
                const synthetic = Object.assign({}, last, {
                    stop_time: nowSec,
                    synthetic_end: true,
                    length: (start ? Math.max(0, nowSec - start) : (last.length || last.elapsed || 0))
                });
                handleCallEnd(synthetic);
            }
            state.missingSince.delete(k);
        }

        state.calls = nextCalls;
        
        // Track call rates per system (using numbered names for consistency)
        const timestamp = Date.now();
        const callsBySys = {};
        nextCalls.forEach(call => {
            // Generate numbered system name: "N. shortname"
            const sysNum = call.sys_num;
            const shortName = call.sys_name || call.short_name || 'Unknown';
            const numberedName = (sysNum !== undefined && sysNum !== null) 
                ? `${sysNum + 1}. ${shortName}` 
                : shortName;
            callsBySys[numberedName] = (callsBySys[numberedName] || 0) + 1;
        });
        
        // Update call rate history for systems with active calls
        Object.keys(callsBySys).forEach(sysName => {
            if (!state.callRateHistory[sysName]) {
                state.callRateHistory[sysName] = [];
            }
            state.callRateHistory[sysName].push({
                time: timestamp,
                count: callsBySys[sysName]
            });
            
            // Keep only last 60 minutes of data
            const oneHourAgo = timestamp - (60 * 60 * 1000);
            state.callRateHistory[sysName] = state.callRateHistory[sysName].filter(
                point => point.time > oneHourAgo
            );
        });
        
        // For systems with no active calls, add zero data point
        state.systems.forEach(sys => {
            const numberedName = `${sys.sys_num + 1}. ${sys.sys_name}`;
            if (!callsBySys[numberedName]) {
                if (!state.callRateHistory[numberedName]) {
                    state.callRateHistory[numberedName] = [];
                }
                state.callRateHistory[numberedName].push({
                    time: timestamp,
                    count: 0
                });
                
                const oneHourAgo = timestamp - (60 * 60 * 1000);
                state.callRateHistory[numberedName] = state.callRateHistory[numberedName].filter(
                    point => point.time > oneHourAgo
                );
            }
        });
        
        updateCallsTable();
        updateStats();
        updateCallRateChart();

        pruneCallCaches();
    });
    
    // Periodic refresh for elapsed time display (every second)
    setInterval(() => {
        if (state.calls.length > 0 || state.stoppedCalls.size > 0) {
            updateCallsTable();
        }
    }, 1000);
    
    eventSource.addEventListener('systems', (e) => {
        const data = JSON.parse(e.data);
        state.systems = data.systems || [];
        // Preserve active system index when updating
        const activeIdx = Array.from(document.querySelectorAll('#systemTabs .tab')).findIndex(t => t.classList.contains('active'));
        updateSystemTabs();
        if (activeIdx >= 0 && activeIdx < state.systems.length) {
            showSystem(activeIdx);
        }
    });

    eventSource.addEventListener('devices', (e) => {
        const data = JSON.parse(e.data);
        state.devices = data.devices || [];
        updateDevicesTiles();
    });
    
    eventSource.addEventListener('rates', (e) => {
        const data = JSON.parse(e.data);
        if (Array.isArray(data.rates)) {
            const timestamp = Date.now();
            data.rates.forEach(r => {
                state.rates[r.sys_name] = r;
                
                // Store rate history
                if (!state.rateHistory[r.sys_name]) {
                    state.rateHistory[r.sys_name] = [];
                }
                state.rateHistory[r.sys_name].push({
                    time: timestamp,
                    rate: r.decoderate
                });
                
                // Keep only last 60 minutes of data (1 sample per 3 seconds = 1200 samples)
                const oneHourAgo = timestamp - (60 * 60 * 1000);
                state.rateHistory[r.sys_name] = state.rateHistory[r.sys_name].filter(
                    point => point.time > oneHourAgo
                );
            });
        }
        updateSystemRates();
        updateChart();
    });
    
    eventSource.addEventListener('config', (e) => {
        const data = JSON.parse(e.data);
        state.config = data.config;
    });
    
    eventSource.addEventListener('console', (e) => {
        const data = JSON.parse(e.data);
        if (data.line) {
            addConsoleLine(data.line);
        }
    });

    eventSource.addEventListener('console_batch', (e) => {
        const data = JSON.parse(e.data);
        if (Array.isArray(data.lines) && data.lines.length) {
            addConsoleLines(data.lines);
        }
        if (data.dropped) {
            addConsoleLine(`[console] dropped ${data.dropped} lines`);
        }
    });
    
    eventSource.addEventListener('console_history', (e) => {
        const data = JSON.parse(e.data);
        if (data.lines && Array.isArray(data.lines)) {
            state.consoleLogs = data.lines;
            hasScrolledConsole = false; // Reset flag so console scrolls to bottom
            renderConsole();
        }
    });
    
    eventSource.addEventListener('call_start', (e) => {
        updateLastUpdate();
    });
    
    eventSource.addEventListener('call_end', (e) => {
        const data = JSON.parse(e.data);
        if (data && data.call) {
            handleCallEnd(data.call);
        }
        updateLastUpdate();
    });
    
    eventSource.addEventListener('recorder', (e) => {
        const data = JSON.parse(e.data);
        if (data.recorder) {
            const idx = state.recorders.findIndex(r => r.id === data.recorder.id);
            if (idx >= 0) {
                state.recorders[idx] = data.recorder;
            } else {
                state.recorders.push(data.recorder);
            }
            updateRecordersTable();
            updateStats();
        }
    });
    
    eventSource.addEventListener('unit_event', (e) => {
        const data = JSON.parse(e.data);
        if (data.event) {
            // Push to messages log
            state.trunkMessages.push(data.event);
            // Trim to buffer size
            if (state.trunkMessages.length > state.omniBufferSize) {
                state.trunkMessages.shift();
            }
            updateOmniMessages();
            
            // Track affiliations
            if (data.event.msg_type === 'AFFILIATION') {
                const key = `${data.event.sys_name}_${data.event.unit}`;
                state.unitAffiliations[key] = {
                    sys_name: data.event.sys_name,
                    unit: data.event.unit,
                    talkgroup: data.event.talkgroup,
                    timestamp: data.event.timestamp
                };
            } else if (data.event.msg_type === 'DEREGISTRATION') {
                const key = `${data.event.sys_name}_${data.event.unit}`;
                delete state.unitAffiliations[key];
            }
        }
    });
}

function initRecentRetentionPicker() {
    const sel = document.getElementById('recentRetention');
    if (!sel) return;

    sel.value = String(state.recentRetention);
    sel.addEventListener('change', () => {
        const next = Number(sel.value);
        state.recentRetention = ([10, 20, 50, 100].includes(next) ? next : 20);
        trimRecentCalls();
        updateCallsTable();
    });
}

function trimRecentCalls() {
    const max = state.recentRetention || 20;
    if (state.recentCalls.length > max) state.recentCalls.length = max;

    if (state.selectedRecentCallKey) {
        const stillThere = state.recentCalls.some(x => x.key === state.selectedRecentCallKey);
        if (!stillThere) state.selectedRecentCallKey = state.recentCalls.length ? state.recentCalls[0].key : null;
    }
}

function updateConnectionStatus() {
    const dot = document.getElementById('connectionStatus');
    const text = document.getElementById('connectionText');
    
    if (state.connected) {
        dot.classList.remove('disconnected');
        text.textContent = 'Connected';
    } else {
        dot.classList.add('disconnected');
        text.textContent = 'Disconnected - Reconnecting...';
    }
}

function updateLastUpdate() {
    const time = new Date().toLocaleTimeString();
    document.getElementById('lastUpdate').textContent = 'Last: ' + time;
    // Make the element fixed-width to prevent shifting
    document.getElementById('lastUpdate').style.minWidth = '140px';
}

function formatFreq(freq) {
    if (!freq) return '-';
    return (freq / 1000000).toFixed(4) + ' MHz';
}

function getStateBadge(stateType, label) {
    const classes = {
        'RECORDING': 'badge-recording',
        'AVAILABLE': 'badge-available',
        'IDLE': 'badge-idle',
        'ACTIVE': 'badge-active',
        'MONITORING': 'badge-active',
        'INACTIVE': 'badge-inactive',
        'STOPPED': 'badge-inactive',
        'IGNORE': 'badge-inactive'
    };
    const cls = classes[stateType] || 'badge-inactive';
    const text = (label !== undefined && label !== null) ? label : (stateType || 'UNKNOWN');
    return `<span class="badge ${cls}">${text}</span>`;
}

function updateRecordersTable() {
    const tbody = document.querySelector('#recordersTable tbody');
    const count = document.getElementById('recorderCount');
    
    count.textContent = state.recorders.length;
    
    // Group recorders by source
    const bySource = {};
    state.recorders.forEach(r => {
        const srcNum = r.src_num || r.srcNum || 0;
        if (!bySource[srcNum]) bySource[srcNum] = [];
        bySource[srcNum].push(r);
    });
    
    // Build table with source separators
    let html = '';
    Object.keys(bySource).sort((a, b) => Number(a) - Number(b)).forEach(srcNum => {
        const recorders = bySource[srcNum];
        
        // Source header row
        html += `
            <tr class="source-separator">
                <td colspan="6" style="background: var(--bg-tertiary); font-weight: bold; padding: 8px; border-top: 2px solid var(--border);">
                    Source ${srcNum} (${recorders.length} recorders)
                </td>
            </tr>
        `;
        
        // Recorder rows
        recorders.forEach(r => {
            const recNum = r.rec_num || r.recNum || 0;
            html += `
                <tr>
                    <td style="padding-left: 20px;">${srcNum}-${recNum}</td>
                    <td>${r.type || '-'}</td>
                    <td class="freq">${formatFreq(r.freq)}</td>
                    <td>${getStateBadge(r.rec_state_type)}</td>
                    <td>${(r.duration || 0).toFixed(1)}s</td>
                    <td>${r.count || 0}</td>
                </tr>
            `;
        });
    });
    
    tbody.innerHTML = html;
    updateLastUpdate();
}

function updateCallsTable() {
    const tbody = document.querySelector('#allCallsTable tbody');
    const count = document.getElementById('activeCallCount');

    if (!tbody) return;

    const calls = getDisplayedCalls();
    const activeCalls = state.calls.length;
    
    // Combine active and recent calls, sorted by call number descending
    const allItems = [];
    
    // Add active/stopped calls
    calls.forEach(c => {
        const k = callKey(c) || '';
        allItems.push({
            key: k,
            call_num: c.call_num || 0,
            isActive: true,
            call: c,
            time: null
        });
    });
    
    // Add recent calls
    state.recentCalls.forEach(item => {
        allItems.push({
            key: item.key,
            call_num: item.call.call_num || 0,
            isActive: false,
            call: item.call,
            time: item.endedAtMs
        });
    });
    
    // Sort by call number descending (highest first)
    allItems.sort((a, b) => b.call_num - a.call_num);
    
    count.textContent = `${activeCalls} active`;

    tbody.innerHTML = allItems.map(item => {
        const c = item.call;
        let rowClass = '';
        if (c.emergency) rowClass = 'emergency';
        else if (c.encrypted) rowClass = 'encrypted';

        const selected = (item.isActive && item.key === state.selectedActiveCallKey) ||
                        (!item.isActive && item.key === state.selectedRecentCallKey);
        
        // Time/Length column: show elapsed for active, length for recent
        let timeLen;
        if (item.isActive) {
            timeLen = `${c.elapsed || 0}s`;
        } else {
            const len = (c.length !== undefined && c.length !== null) ? Number(c.length).toFixed(1) : '-';
            timeLen = `${len}s`;
        }
        
        // State column
        let stateHtml;
        if (item.isActive) {
            stateHtml = getStateBadge(
                c.call_state_type,
                `${c.call_state_type || 'UNKNOWN'}${c.encrypted ? ' [E]' : ''}`
            );
        } else {
            stateHtml = c.encrypted ? 'Complete [E]' : 'Complete';
        }
        
        const dataAttr = item.isActive ? `data-active-key="${item.key}"` : `data-recent-key="${item.key}"`;
        
        // Format frequency - always show if available, even for completed calls
        const freqDisplay = c.freq ? formatFreq(c.freq) : '-';
        
        return `
            <tr class="${rowClass} ${selected ? 'selected' : ''}" ${dataAttr}>
                <td>${c.call_num || '-'}</td>
                <td>${c.sys_name || c.short_name || '-'}</td>
                <td class="tg-num">${c.talkgroup || '-'}</td>
                <td>${c.talkgroup_alpha_tag || c.talkgroup_alpha || '-'}</td>
                <td class="freq">${freqDisplay}</td>
                <td>${c.unit || c.srcId || '-'}</td>
                <td>${timeLen}</td>
                <td>${stateHtml}</td>
            </tr>
        `;
    }).join('');
    
    updateLastUpdate();
    updateDetailsPane();
}

function callKey(call) {
    if (!call) return null;
    if (call.call_num !== undefined && call.call_num !== null) return String(call.call_num);
    if (call.callNum !== undefined && call.callNum !== null) return String(call.callNum);
    if (call.id !== undefined && call.id !== null) return String(call.id);

    // Fallback: stable composite key (best-effort)
    const sys = call.sys_name || call.short_name || call.sysName || '';
    const tg = (call.talkgroup !== undefined && call.talkgroup !== null) ? String(call.talkgroup) : '';
    const freq = (call.freq !== undefined && call.freq !== null) ? String(call.freq) : '';
    const st = (call.start_time !== undefined && call.start_time !== null) ? String(call.start_time)
        : (call.startTime !== undefined && call.startTime !== null) ? String(call.startTime)
        : '';
    const unit = (call.unit !== undefined && call.unit !== null) ? String(call.unit)
        : (call.srcId !== undefined && call.srcId !== null) ? String(call.srcId)
        : '';

    const composite = [sys, tg, freq, st, unit].join('|');
    return composite === '||||' ? null : composite;
}

function pruneStoppedCalls() {
    const now = Date.now();
    const toMove = [];
    
    // Collect expired stopped calls to move to recent
    for (const [key, entry] of state.stoppedCalls.entries()) {
        if (entry.expireAt <= now) {
            // Check if this was an IGNORED call
            const wasIgnored = entry.call && entry.call.call_state_type === 'IGNORE';
            if (!wasIgnored) {
                toMove.push({ call: entry.call, rawJson: entry.rawJson });
            }
            state.stoppedCalls.delete(key);
        }
    }
    
    // Move to recent calls (outside the iteration)
    toMove.forEach(item => {
        const endedAtMs = (item.rawJson && item.rawJson.stop_time) ? (Number(item.rawJson.stop_time) * 1000) : Date.now();
        const recentItem = {
            key: callKey(item.rawJson || item.call) || String(Date.now()),
            endedAtMs,
            call: normalizeCallSummary(item.rawJson || item.call, item.call),
            json: item.rawJson || item.call
        };
        state.recentCalls.unshift(recentItem);
    });
    
    if (toMove.length > 0) {
        trimRecentCalls();
    }
}

function getDisplayedCalls() {
    const now = Date.now();
    
    const activeKeys = new Set(state.calls.map(callKey).filter(Boolean));
    // If a call is active again, remove any stopped record
    for (const k of activeKeys) {
        if (state.stoppedCalls.has(k)) state.stoppedCalls.delete(k);
    }

    const stopped = Array.from(state.stoppedCalls.values())
        .filter(e => e.expireAt > now)
        .map(e => e.call);

    return [...state.calls, ...stopped];
}

function handleCallEnd(callEndJson) {
    const key = callKey(callEndJson);
    if (!key) return;

    // Suppress duplicates (e.g., synthetic end followed by real call_end).
    const now = Date.now();
    const alreadyEndedAt = state.endedKeyTimes.get(key);
    if (alreadyEndedAt !== undefined && (now - alreadyEndedAt) < 3000) {
        // Already processed this call end recently
        state.missingSince.delete(key);
        return;
    }

    markEndedKey(key);
    state.missingSince.delete(key);

    // Try to enrich with last-seen active call fields
    const active = state.calls.find(c => callKey(c) === key) || state.lastActiveByKey.get(key);
    const enriched = Object.assign({}, active || {}, callEndJson);

    // Check if this was an IGNORED call
    const wasIgnored = (active && active.call_state_type === 'IGNORE') || 
                       (callEndJson && callEndJson.call_state_type === 'IGNORE');

    // Normalize fields used by the Active Calls table
    // Preserve IGNORE state, otherwise set to STOPPED
    if (!wasIgnored) {
        enriched.call_state_type = 'STOPPED';
    }
    enriched.elapsed = Math.round(enriched.length || enriched.elapsed || 0);

    state.stoppedCalls.set(key, {
        expireAt: now + 3000,  // Show in active list for 3 seconds before moving to recent
        call: enriched,
        rawJson: Object.assign({}, active || {}, callEndJson)
    });

    updateCallsTable();
}

function pushRecentCall(call, rawJson) {
    const endedAtMs = (rawJson && rawJson.stop_time) ? (Number(rawJson.stop_time) * 1000) : Date.now();
    const item = {
        key: callKey(rawJson || call) || String(Date.now()),
        endedAtMs,
        call: normalizeCallSummary(rawJson || call, call),
        json: rawJson || call
    };

    state.recentCalls.unshift(item);
    trimRecentCalls();

    if (!state.selectedRecentCallKey) {
        state.selectedRecentCallKey = item.key;
    }
}

function normalizeCallSummary(primaryJson, fallbackActive) {
    // primaryJson may be either the active-call JSON (our own) or trunk-recorder's call_json.
    const j = primaryJson || {};

    const sys = j.sys_name || j.short_name || (fallbackActive && fallbackActive.sys_name) || '-';
    const tg = j.talkgroup || (fallbackActive && fallbackActive.talkgroup) || '-';
    const alpha = j.talkgroup_alpha_tag || j.talkgroup_tag || (fallbackActive && fallbackActive.talkgroup_alpha_tag) || '-';

    let unit = j.unit || '-';
    if ((!unit || unit === '-') && Array.isArray(j.srcList) && j.srcList.length) {
        const last = j.srcList[j.srcList.length - 1];
        unit = (last && (last.tag || last.src)) ? (last.tag || String(last.src)) : unit;
    }

    const enc = (j.encrypted === true) || (j.encrypted === 1) || (fallbackActive && fallbackActive.encrypted);
    const length = (j.length !== undefined) ? Number(j.length)
        : (j.call_length !== undefined) ? Number(j.call_length)
        : (fallbackActive && fallbackActive.length !== undefined) ? Number(fallbackActive.length)
        : null;
    
    const callNum = j.call_num || (fallbackActive && fallbackActive.call_num) || null;
    const freq = j.freq || (fallbackActive && fallbackActive.freq) || null;

    return {
        call_num: callNum,
        sys_name: sys,
        talkgroup: tg,
        talkgroup_alpha_tag: alpha,
        unit: unit,
        encrypted: !!enc,
        length: length,
        freq: freq
    };
}

function updateDetailsPane() {
    const view = document.getElementById('detailsPane');
    if (!view) return;

    const renderTable = (json) => {
        if (!json || typeof json !== 'object') {
            return '<div class="details-empty">Select a call to view details</div>';
        }
        
        const rows = [];
        const keys = Object.keys(json);
        
        for (const key of keys) {
            const value = json[key];
            let valueHtml = '';
            
            // Handle different value types
            if (value === null || value === undefined) {
                valueHtml = '<span style="color: var(--text-secondary);">-</span>';
            } else if (key === 'freqList' && Array.isArray(value)) {
                // Special handling for frequency list
                if (value.length === 0) {
                    valueHtml = '<span style="color: var(--text-secondary);">-</span>';
                } else {
                    const freqItems = value.map((item) => {
                        const freq = item.freq ? `${(item.freq / 1e6).toFixed(4)} MHz` : '-';
                        const time = item.time ? `${item.time.toFixed(2)}s` : '-';
                        const spike = item.spike_count !== undefined ? ` (spikes: ${item.spike_count})` : '';
                        return `<li>${freq} @ ${time}${spike}</li>`;
                    }).join('');
                    valueHtml = `<ul class="details-sublist">${freqItems}</ul>`;
                }
            } else if (key === 'srcList' && Array.isArray(value)) {
                // Special handling for source list (transmissions)
                if (value.length === 0) {
                    valueHtml = '<span style="color: var(--text-secondary);">-</span>';
                } else {
                    const srcItems = value.map((item) => {
                        const src = item.src !== undefined ? item.src : (item.srcId !== undefined ? item.srcId : '-');
                        const pos = item.pos !== undefined ? `${item.pos.toFixed(2)}s` : '-';
                        const len = item.length !== undefined ? `${item.length.toFixed(2)}s` : '-';
                        const emg = item.emergency ? ' [EMERGENCY]' : '';
                        return `<li>Unit ${src} @ ${pos} (${len})${emg}</li>`;
                    }).join('');
                    valueHtml = `<ul class="details-sublist">${srcItems}</ul>`;
                }
            } else if (Array.isArray(value)) {
                // Generic array handling
                if (value.length === 0) {
                    valueHtml = '[]';
                } else {
                    valueHtml = `[${value.length} items]`;
                }
            } else if (typeof value === 'object') {
                // Nested object
                valueHtml = JSON.stringify(value, null, 2);
            } else if (typeof value === 'boolean') {
                valueHtml = value ? 'Yes' : 'No';
            } else if (typeof value === 'number') {
                // Format numbers nicely
                if (key.includes('freq') || key === 'freq') {
                    valueHtml = `${(value / 1e6).toFixed(4)} MHz`;
                } else if (key.includes('time') && value > 1000000000) {
                    // Looks like a timestamp
                    valueHtml = new Date(value * 1000).toLocaleString();
                } else {
                    valueHtml = value.toLocaleString();
                }
            } else {
                valueHtml = escapeHtml(String(value));
            }
            
            // Friendly key names
            const friendlyKey = key
                .replace(/_/g, ' ')
                .replace(/([a-z])([A-Z])/g, '$1 $2')
                .replace(/\b\w/g, l => l.toUpperCase());
            
            rows.push(`
                <tr>
                    <td>${escapeHtml(friendlyKey)}</td>
                    <td>${valueHtml}</td>
                </tr>
            `);
        }
        
        if (rows.length === 0) {
            return '<div class="details-empty">No details available</div>';
        }
        
        return `<table class="details-table"><tbody>${rows.join('')}</tbody></table>`;
    };

    // Prefer an explicitly selected active call; otherwise selected recent call.
    let json = null;
    if (state.selectedActiveCallKey) {
        const c = state.lastActiveByKey.get(state.selectedActiveCallKey)
            || state.calls.find(x => callKey(x) === state.selectedActiveCallKey);
        json = c;
    } else if (state.selectedRecentCallKey) {
        const item = state.recentCalls.find(x => x.key === state.selectedRecentCallKey);
        json = item ? item.json : null;
    }

    try {
        view.innerHTML = renderTable(json);
    } catch (err) {
        console.error('Error rendering details:', err);
        view.innerHTML = '<div class="details-empty">Error rendering details</div>';
    }
}

function updateStats() {
    document.getElementById('statSystems').textContent = state.systems.length;
    document.getElementById('statRecorders').textContent = state.recorders.length;
    document.getElementById('statActiveCalls').textContent = state.calls.length;
    document.getElementById('statRecording').textContent = 
        state.recorders.filter(r => r.rec_state_type === 'RECORDING').length;
}

function updateSystemTabs() {
    const tabs = document.getElementById('systemTabs');
    const panels = document.getElementById('systemPanels');
    
    tabs.innerHTML = state.systems.map((s, i) => `
        <div class="tab ${i === 0 ? 'active' : ''}" 
             onclick="showSystem(${i})">${s.sys_name}</div>
    `).join('');
    
    panels.innerHTML = state.systems.map((s, i) => `
        <div class="system-panel ${i === 0 ? 'active' : ''}" id="system-${i}">
            <div class="system-split">
                <div class="system-left">
                    <div class="stats-row">
                        <div class="stat-box">
                            <div class="stat-value">${s.sysid || '-'}</div>
                            <div class="stat-label">SysID</div>
                        </div>
                        <div class="stat-box">
                            <div class="stat-value" id="rate-${s.sys_name}">-</div>
                            <div class="stat-label">Decode Rate</div>
                        </div>
                        <div class="stat-box">
                            <div class="stat-value">${s.wacn || '-'}</div>
                            <div class="stat-label">WACN</div>
                        </div>
                        <div class="stat-box">
                            <div class="stat-value">${s.nac || '-'}</div>
                            <div class="stat-label">NAC</div>
                        </div>
                    </div>
                    <table>
                        <tr><th>Type</th><td>${s.type || '-'}</td></tr>
                        <tr><th>RFSS</th><td>${s.rfss || '-'}</td></tr>
                        <tr><th>Site ID</th><td>${s.site_id || '-'}</td></tr>
                    </table>
                    <h4>Control Channels</h4>
                    <div class="control-channels">
                        ${(Array.isArray(s.control_channels) && s.control_channels.length) ?
                            s.control_channels.map(cc => `
                                <div class="cc-item ${cc === s.control_channel ? 'active' : ''}">
                                    ${formatFreq(cc)}
                                </div>
                            `).join('') :
                            `<div class="cc-item active">${formatFreq(s.control_channel)}</div>`
                        }
                    </div>
                    <h4>Data</h4>
                    <div class="data-links">
                        <button class="data-link" onclick="loadSystemData(${i}, 'talkgroups')">Talkgroups</button>
                        <button class="data-link" onclick="loadSystemData(${i}, 'unit_tags')">Unit Tags</button>
                        <button class="data-link" onclick="loadSystemData(${i}, 'unit_tags_ota')">OTA Aliases</button>
                    </div>
                </div>
                <div class="system-right">
                    <div class="system-display" id="system-display-${i}">Select a data type to display</div>
                </div>
            </div>
        </div>
    `).join('');
    
    updateStats();
}

function fmtNumber(v, digits) {
    if (v === undefined || v === null || Number.isNaN(Number(v))) return '-';
    return Number(v).toFixed(digits);
}

function updateDevicesTiles() {
    const grid = document.getElementById('devicesGrid');
    const count = document.getElementById('deviceCount');
    if (!grid || !count) return;

    count.textContent = state.devices.length;

    grid.innerHTML = state.devices.map(d => {
        const deviceNum = (d.src_num !== undefined && d.src_num !== null) ? d.src_num : '-';
        const driver = d.driver || '-';
        const freq = formatFreq(d.center);
        const deviceStr = (d.device !== undefined && d.device !== null && String(d.device).trim() !== '') ? String(d.device) : '-';
        const rate = d.rate ? (d.rate / 1000000).toFixed(3) + ' Msps' : '-';
        const gain = (d.gain !== undefined && d.gain !== null) ? fmtNumber(d.gain, 1) : '-';
        const analog = (d.analog_recorders ?? '-');
        const digital = (d.digital_recorders ?? '-');
        const errHz = (d.error !== undefined && d.error !== null) ? `${fmtNumber(d.error, 0)} Hz` : '-';
        const autotuneHz = d.autotune_enabled ? `${(d.autotune_offset_hz ?? 0)} Hz` : 'false';

        const extraGains = Array.isArray(d.gain_stages) ? d.gain_stages : [];
        const extraGainRows = extraGains.map(gs => {
            const name = (gs && gs.name) ? String(gs.name) : 'Gain';
            const val = (gs && gs.value !== undefined && gs.value !== null) ? fmtNumber(gs.value, 1) : '-';
            return `
                <div class="device-line span-2">
                    <span class="k">${name}:</span>
                    <span class="v">${val}</span>
                </div>
            `;
        }).join('');

        return `
            <div class="device-tile">
                <div class="device-title">
                    <span class="device-title-main">Device ${deviceNum}</span>
                    <span class="device-title-meta">[${driver}]</span>
                </div>

                <div class="device-lines">
                    <div class="device-line span-2">
                        <span class="k">Frequency:</span>
                        <span class="v v-freq">${freq}</span>
                    </div>

                    <div class="device-line span-2">
                        <span class="k">Device:</span>
                        <span class="v v-wrap">${deviceStr}</span>
                    </div>

                    <div class="device-line">
                        <span class="k">Rate:</span>
                        <span class="v">${rate}</span>
                    </div>
                    <div class="device-line">
                        <span class="k">Gain:</span>
                        <span class="v">${gain}</span>
                    </div>

                    <div class="device-line">
                        <span class="k">AnalogRecorders:</span>
                        <span class="v">${analog}</span>
                    </div>
                    <div class="device-line">
                        <span class="k">DigitalRecorders:</span>
                        <span class="v">${digital}</span>
                    </div>

                    <div class="device-line">
                        <span class="k">Error:</span>
                        <span class="v">${errHz}</span>
                    </div>
                    <div class="device-line">
                        <span class="k">Autotune:</span>
                        <span class="v">${autotuneHz}</span>
                    </div>

                    ${extraGainRows}
                </div>
            </div>
        `;
    }).join('');
}

function updateSystemRates() {
    Object.keys(state.rates).forEach(sysName => {
        const el = document.getElementById('rate-' + sysName);
        if (el) {
            el.textContent = (state.rates[sysName].decoderate || 0).toFixed(1);
        }
    });
}

function showSystem(index) {
    document.querySelectorAll('#systemTabs .tab').forEach((t, i) => {
        t.classList.toggle('active', i === index);
    });
    document.querySelectorAll('.system-panel').forEach((p, i) => {
        p.classList.toggle('active', i === index);
    });
}

function loadSystemData(index, type) {
    const sys = state.systems[index];
    if (!sys) return;

    const display = document.getElementById(`system-display-${index}`);
    if (!display) return;

    display.innerHTML = '<div class="loading">Loading...</div>';

    fetch(`${BASE_PATH}api/system/${type}?sys_num=${sys.sys_num}`, {
        credentials: 'include'
    })
        .then(r => r.json())
        .then(data => {
            if (type === 'talkgroups') {
                if (Array.isArray(data) && data.length) {
                    display.innerHTML = `
                        <table>
                            <thead>
                                <tr>
                                    <th>Number</th>
                                    <th>Alpha Tag</th>
                                    <th>Description</th>
                                    <th>Tag</th>
                                    <th>Group</th>
                                    <th>Priority</th>
                                </tr>
                            </thead>
                            <tbody>
                                ${data.map(tg => `
                                    <tr>
                                        <td class="tg-num">${tg.number}</td>
                                        <td>${tg.alpha_tag || '-'}</td>
                                        <td>${tg.description || '-'}</td>
                                        <td>${tg.tag || '-'}</td>
                                        <td>${tg.group || '-'}</td>
                                        <td>${tg.priority}</td>
                                    </tr>
                                `).join('')}
                            </tbody>
                        </table>
                    `;
                } else {
                    display.innerHTML = '<p>No talkgroups configured</p>';
                }
            } else if (type === 'unit_tags') {
                if (data.tags && Array.isArray(data.tags) && data.tags.length) {
                    display.innerHTML = `
                        <p><strong>File:</strong> ${data.file || 'None'}</p>
                        <p><strong>Mode:</strong> ${data.mode || 'default'}</p>
                        <p><strong>Count:</strong> ${data.count || 0}</p>
                        <table style="margin-top: 1rem;">
                            <thead>
                                <tr>
                                    <th>Pattern (Regex)</th>
                                    <th>Tag</th>
                                </tr>
                            </thead>
                            <tbody>
                                ${data.tags.map(t => `
                                    <tr>
                                        <td style="font-family: 'Courier New', monospace;">${t.pattern || ''}</td>
                                        <td>${t.tag || ''}</td>
                                    </tr>
                                `).join('')}
                            </tbody>
                        </table>
                    `;
                } else {
                    display.innerHTML = `
                        <p><strong>File:</strong> ${data.file || 'None'}</p>
                        <p><strong>Mode:</strong> ${data.mode || 'default'}</p>
                        <p style="margin-top: 1rem;">No manual unit tags defined.</p>
                    `;
                }
            } else if (type === 'unit_tags_ota') {
                if (Array.isArray(data.aliases) && data.aliases.length) {
                    display.innerHTML = `
                        <p><strong>OTA File:</strong> ${data.file || 'None'}</p>
                        <p><strong>Count:</strong> ${data.count || 0}</p>
                        <table style="margin-top: 1rem;">
                            <thead>
                                <tr>
                                    <th>Unit ID</th>
                                    <th>Alias</th>
                                </tr>
                            </thead>
                            <tbody>
                                ${data.aliases.map(a => `
                                    <tr>
                                        <td>${a.unit}</td>
                                        <td>${a.alias}</td>
                                    </tr>
                                `).join('')}
                            </tbody>
                        </table>
                    `;
                } else {
                    display.innerHTML = `<p><strong>OTA File:</strong> ${data.file || 'None'}</p><p>No OTA aliases available</p>`;
                }
            }
        })
        .catch(err => {
            display.innerHTML = `<p class="error">Error loading data: ${err.message}</p>`;
        });
}

// Main tab navigation
function showMainTab(tabName) {
    document.querySelectorAll('.main-tab').forEach(t => {
        t.classList.toggle('active', t.dataset.tab === tabName);
    });
    document.querySelectorAll('.main-panel').forEach(p => {
        p.classList.toggle('active', p.id === 'panel-' + tabName);
    });
    
    if (tabName === 'status') {
        updateChart();
        updateCallRateChart();
    } else if (tabName === 'console') {
        // Scroll console to bottom when tab is opened
        setTimeout(() => {
            const container = document.getElementById('consoleOutput');
            if (container) {
                container.scrollTop = container.scrollHeight;
            }
        }, 0);
    } else if (tabName === 'omnitrunker') {
        // Initialize Omnitrunker when tab is opened
        setTimeout(() => {
            initOmnitrunker();
            updateVoiceChannels();
            updateOmniMessages();
        }, 0);
    } else if (tabName === 'affiliations') {
        // Load affiliations when tab is opened
        setTimeout(() => {
            loadAffiliations();
        }, 0);
    } else if (tabName === 'admin') {
        // Load admin data when tab is opened
        setTimeout(() => {
            showAdminSection('status');  // Show System Status by default
            // Ensure editor line numbers initialize properly
            const editor = document.getElementById('configEditor');
            if (editor && typeof updateLineNumbers === 'function') {
                setTimeout(updateLineNumbers, 50);
            }
        }, 0);
    }
}

function initWrapToggle() {
    const cb = document.getElementById('wrapToggle');
    const container = document.getElementById('consoleOutput');
    if (!cb || !container) return;
    const apply = () => {
        container.classList.toggle('nowrap', !cb.checked);
    };
    cb.addEventListener('change', apply);
    apply();
}

// Console functionality
function addConsoleLine(line) {
    state.consoleLogs.push(line);
    
    // Trim to max lines (respects backend config)
    const maxLines = state.consoleMaxLines || 5000;
    if (state.consoleLogs.length > maxLines) {
        state.consoleLogs = state.consoleLogs.slice(-maxLines);
    }
    
    const container = document.getElementById('consoleOutput');
    if (container) {
        const lineEl = document.createElement('div');
        lineEl.className = 'console-line';
        lineEl.innerHTML = formatConsoleLine(line);
        container.appendChild(lineEl);
        
        // Auto-scroll if near bottom
        if (container.scrollHeight - container.scrollTop - container.clientHeight < 100) {
            container.scrollTop = container.scrollHeight;
        }
        
        // Trim DOM elements if too many (respects backend config)
        while (container.children.length > maxLines) {
            container.removeChild(container.firstChild);
        }
    }
}

function addConsoleLines(lines) {
    if (!Array.isArray(lines) || !lines.length) return;

    const maxLines = state.consoleMaxLines || 5000;

    // Update state (keep only last N)
    state.consoleLogs.push(...lines);
    if (state.consoleLogs.length > maxLines) {
        state.consoleLogs = state.consoleLogs.slice(-maxLines);
    }

    const container = document.getElementById('consoleOutput');
    if (!container) return;

    const shouldScroll = (container.scrollHeight - container.scrollTop - container.clientHeight) < 100;
    const frag = document.createDocumentFragment();
    for (const line of lines) {
        const lineEl = document.createElement('div');
        lineEl.className = 'console-line';
        lineEl.innerHTML = formatConsoleLine(line);
        frag.appendChild(lineEl);
    }
    container.appendChild(frag);

    if (shouldScroll) {
        container.scrollTop = container.scrollHeight;
    }

    while (container.children.length > maxLines) {
        container.removeChild(container.firstChild);
    }
}

function renderConsole() {
    const container = document.getElementById('consoleOutput');
    if (!container) return;
    
    // Check if user was at bottom before render
    const wasAtBottom = container.scrollHeight - container.scrollTop - container.clientHeight < 100;
    
    container.innerHTML = state.consoleLogs.map(line => 
        `<div class="console-line">${formatConsoleLine(line)}</div>`
    ).join('');
    
    // Always scroll to bottom on first render, then follow user position
    if (!hasScrolledConsole || wasAtBottom) {
        hasScrolledConsole = true;
        // Use requestAnimationFrame to ensure DOM has updated
        requestAnimationFrame(() => {
            container.scrollTop = container.scrollHeight;
        });
    }
}

function jumpToBottom() {
    const container = document.getElementById('consoleOutput');
    if (container) {
        container.scrollTop = container.scrollHeight;
    }
}

// ANSI to HTML converter
function ansiToHtml(text) {
    // Some log pipelines double-escape ANSI sequences (e.g. literal "\\u001b[...m").
    // Normalize those to real ESC so the parser can apply colors.
    text = text
        .replace(/\\u001b/g, '\u001b')
        .replace(/\\x1b/g, '\u001b')
        .replace(/\\033/g, '\u001b');

    // Escape HTML first
    text = text.replace(/&/g, '&amp;')
               .replace(/</g, '&lt;')
               .replace(/>/g, '&gt;');
    
    // ANSI color code mapping
    const ansiColors = {
        '30': 'ansi-black',
        '31': 'ansi-red',
        '32': 'ansi-green',
        '33': 'ansi-yellow',
        '34': 'ansi-blue',
        '35': 'ansi-magenta',
        '36': 'ansi-cyan',
        '37': 'ansi-white',
        '90': 'ansi-bright-black',
        '91': 'ansi-bright-red',
        '92': 'ansi-bright-green',
        '93': 'ansi-bright-yellow',
        '94': 'ansi-bright-blue',
        '95': 'ansi-bright-magenta',
        '96': 'ansi-bright-cyan',
        '97': 'ansi-bright-white',
        '1': 'ansi-bold',
        '2': 'ansi-dim',
        '3': 'ansi-italic',
        '4': 'ansi-underline'
    };

    function xterm256ToRgb(n) {
        if (Number.isNaN(n) || n < 0) return null;
        if (n <= 15) {
            // Standard + high-intensity palette
            const base = [
                [0, 0, 0],       [205, 0, 0],     [0, 205, 0],   [205, 205, 0],
                [0, 0, 238],     [205, 0, 205],   [0, 205, 205], [229, 229, 229],
                [127, 127, 127], [255, 0, 0],     [0, 255, 0],   [255, 255, 0],
                [92, 92, 255],   [255, 0, 255],   [0, 255, 255], [255, 255, 255]
            ];
            return base[n] || null;
        }
        if (n >= 16 && n <= 231) {
            const idx = n - 16;
            const r = Math.floor(idx / 36);
            const g = Math.floor((idx % 36) / 6);
            const b = idx % 6;
            const steps = [0, 95, 135, 175, 215, 255];
            return [steps[r], steps[g], steps[b]];
        }
        if (n >= 232 && n <= 255) {
            const v = 8 + (n - 232) * 10;
            return [v, v, v];
        }
        return null;
    }
    
    let result = '';
    let openSpans = 0;
    
    // Parse ANSI escape sequences
    const regex = /\u001b\[([0-9;]*)m/g;
    let lastIndex = 0;
    let match;
    
    while ((match = regex.exec(text)) !== null) {
        // Add text before this match
        result += text.slice(lastIndex, match.index);
        lastIndex = regex.lastIndex;
        
        const raw = match[1].trim();
        const codes = raw.length ? raw.split(';').filter(c => c !== '') : [];

        // Treat empty sequence or any explicit reset as a full reset
        if (codes.length === 0 || codes.includes('0')) {
            while (openSpans > 0) {
                result += '</span>';
                openSpans--;
            }
        }

        // Apply remaining codes (ignore 0 if present)
        for (let i = 0; i < codes.length; i++) {
            const code = codes[i];
            if (code === '0') continue;

            // Some logs use 39/49 to reset fg/bg; we don't track state, so do a full reset
            if (code === '39' || code === '49') {
                while (openSpans > 0) {
                    result += '</span>';
                    openSpans--;
                }
                continue;
            }

            // 256-color / truecolor (foreground/background)
            if (code === '38' || code === '48') {
                const isBg = (code === '48');
                const mode = codes[i + 1];
                if (mode === '5') {
                    const n = parseInt(codes[i + 2], 10);
                    const rgb = xterm256ToRgb(n);
                    if (rgb) {
                        const style = isBg
                            ? `background-color: rgb(${rgb[0]}, ${rgb[1]}, ${rgb[2]})`
                            : `color: rgb(${rgb[0]}, ${rgb[1]}, ${rgb[2]})`;
                        result += `<span style="${style}">`;
                        openSpans++;
                    }
                    i += 2;
                    continue;
                }
                if (mode === '2') {
                    const r = parseInt(codes[i + 2], 10);
                    const g = parseInt(codes[i + 3], 10);
                    const b = parseInt(codes[i + 4], 10);
                    if ([r, g, b].every(v => Number.isFinite(v) && v >= 0 && v <= 255)) {
                        const style = isBg
                            ? `background-color: rgb(${r}, ${g}, ${b})`
                            : `color: rgb(${r}, ${g}, ${b})`;
                        result += `<span style="${style}">`;
                        openSpans++;
                    }
                    i += 4;
                    continue;
                }
                continue;
            }

            if (ansiColors[code]) {
                result += `<span class="${ansiColors[code]}">`;
                openSpans++;
            }
        }
    }
    
    // Add remaining text
    result += text.slice(lastIndex);
    
    // Close any open spans
    while (openSpans > 0) {
        result += '</span>';
        openSpans--;
    }
    
    return result;
}

function formatConsoleLine(line) {
    // Extract a severity prefix if present: [info] [warning] [error]
    // (Inserted server-side; if missing, we just render the line.)
    const m = /^\[(trace|debug|info|warning|error|fatal)\]\s*/i.exec(line);
    if (!m) {
        return ansiToHtml(line);
    }

    const sev = m[1].toLowerCase();
    const rest = line.slice(m[0].length);

    let cls = 'sev-info';
    if (sev === 'warning') cls = 'sev-warning';
    if (sev === 'error' || sev === 'fatal') cls = 'sev-error';

    const sevHtml = `<span class="sev ${cls}">[${sev}]</span>`;
    return sevHtml + ansiToHtml(rest);
}

// Chart functionality
let currentChartPeriod = '5m';
let currentCallChartPeriod = '5m';
let chartCanvas = null;
let chartCtx = null;
let callRateChartCanvas = null;
let callRateChartCtx = null;

function initChart() {
    chartCanvas = document.getElementById('rateChart');
    if (chartCanvas) {
        chartCtx = chartCanvas.getContext('2d');
    }
    
    callRateChartCanvas = document.getElementById('callRateChart');
    if (callRateChartCanvas) {
        callRateChartCtx = callRateChartCanvas.getContext('2d');
    }
}

function setChartPeriod(period) {
    currentChartPeriod = period;
    document.querySelectorAll('.chart-tab[data-chart="rate"]').forEach(t => {
        t.classList.toggle('active', t.dataset.period === period);
    });
    updateChart();
}

function setCallChartPeriod(period) {
    currentCallChartPeriod = period;
    document.querySelectorAll('.chart-tab[data-chart="calls"]').forEach(t => {
        t.classList.toggle('active', t.dataset.period === period);
    });
    updateCallRateChart();
}

function updateChart() {
    if (!chartCtx) return;
    
    const canvas = chartCanvas;
    const ctx = chartCtx;
    
    // Set canvas size
    const container = canvas.parentElement;
    canvas.width = container.clientWidth;
    canvas.height = container.clientHeight;
    
    const width = canvas.width;
    const height = canvas.height;
    const padding = { top: 20, right: 20, bottom: 40, left: 50 };
    const chartWidth = width - padding.left - padding.right;
    const chartHeight = height - padding.top - padding.bottom;
    
    // Clear canvas (theme-driven)
    ctx.fillStyle = cssVar('--bg-secondary', '#16213e');
    ctx.fillRect(0, 0, width, height);
    
    // Determine time range
    const now = Date.now();
    let timeRange;
    switch (currentChartPeriod) {
        case '5m': timeRange = 5 * 60 * 1000; break;
        case '15m': timeRange = 15 * 60 * 1000; break;
        case '60m': timeRange = 60 * 60 * 1000; break;
        default: timeRange = 5 * 60 * 1000;
    }
    const startTime = now - timeRange;
    
    // Y-axis: 0-40 fixed
    const yMin = 0;
    const yMax = 40;
    
    // Draw grid
    ctx.strokeStyle = cssVar('--border', '#2a2a4e');
    ctx.lineWidth = 1;
    
    // Horizontal grid lines
    for (let y = 0; y <= 40; y += 10) {
        const yPos = padding.top + chartHeight - (y / yMax * chartHeight);
        ctx.beginPath();
        ctx.moveTo(padding.left, yPos);
        ctx.lineTo(width - padding.right, yPos);
        ctx.stroke();
        
        // Y-axis labels
        ctx.fillStyle = cssVar('--text-secondary', '#a0a0a0');
        ctx.font = '11px sans-serif';
        ctx.textAlign = 'right';
        ctx.fillText(y.toString(), padding.left - 5, yPos + 4);
    }
    
    // Time labels
    ctx.fillStyle = cssVar('--text-secondary', '#a0a0a0');
    ctx.font = '11px sans-serif';
    ctx.textAlign = 'center';
    const timeLabels = currentChartPeriod === '5m' ? 5 : currentChartPeriod === '15m' ? 5 : 6;
    for (let i = 0; i <= timeLabels; i++) {
        const t = startTime + (timeRange * i / timeLabels);
        const x = padding.left + (chartWidth * i / timeLabels);
        const date = new Date(t);
        ctx.fillText(date.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'}), x, height - 10);
    }
    
    // Y-axis title
    ctx.save();
    ctx.translate(15, height / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.textAlign = 'center';
    ctx.fillStyle = cssVar('--text-secondary', '#a0a0a0');
    ctx.fillText('Decode Rate (msg/sec)', 0, 0);
    ctx.restore();
    
    // Draw data lines for each system
    const systems = Object.keys(state.rateHistory);
    const legendContainer = document.getElementById('chartLegend');
    if (legendContainer) {
        legendContainer.innerHTML = '';
    }
    
    // Initialize hidden systems set if not exists
    if (!state.hiddenRateSystems) state.hiddenRateSystems = new Set();
    
    const palette = getChartPalette();

    systems.forEach((sysName, idx) => {
        const data = state.rateHistory[sysName].filter(p => p.time >= startTime);
        const color = palette[idx % palette.length];
        const isHidden = state.hiddenRateSystems.has(sysName);
        
        // Draw line if not hidden and has enough data
        if (!isHidden && data.length >= 2) {
            ctx.strokeStyle = color;
            ctx.lineWidth = 2;
            ctx.beginPath();
            
            data.forEach((point, i) => {
                const x = padding.left + ((point.time - startTime) / timeRange * chartWidth);
                const y = padding.top + chartHeight - (point.rate / yMax * chartHeight);
                
                if (i === 0) {
                    ctx.moveTo(x, y);
                } else {
                    ctx.lineTo(x, y);
                }
            });
            
            ctx.stroke();
        }
        
        // Add to legend (always show, even if hidden)
        if (legendContainer) {
            const item = document.createElement('div');
            item.className = 'legend-item' + (isHidden ? ' legend-hidden' : '');
            item.style.cursor = 'pointer';
            item.style.opacity = isHidden ? '0.4' : '1';
            item.innerHTML = `<div class="legend-color" style="background: ${color}"></div>${sysName}`;
            item.onclick = () => {
                if (state.hiddenRateSystems.has(sysName)) {
                    state.hiddenRateSystems.delete(sysName);
                } else {
                    state.hiddenRateSystems.add(sysName);
                }
                updateChart();
            };
            legendContainer.appendChild(item);
        }
    });
}

function updateCallRateChart() {
    if (!callRateChartCtx) return;
    
    const canvas = callRateChartCanvas;
    const ctx = callRateChartCtx;
    
    // Set canvas size
    const container = canvas.parentElement;
    canvas.width = container.clientWidth;
    canvas.height = container.clientHeight;
    
    const width = canvas.width;
    const height = canvas.height;
    const padding = { top: 20, right: 20, bottom: 40, left: 50 };
    const chartWidth = width - padding.left - padding.right;
    const chartHeight = height - padding.top - padding.bottom;
    
    // Clear canvas (theme-driven)
    ctx.fillStyle = cssVar('--bg-secondary', '#16213e');
    ctx.fillRect(0, 0, width, height);
    
    // Determine time range
    const now = Date.now();
    let timeRange;
    switch (currentCallChartPeriod) {
        case '5m': timeRange = 5 * 60 * 1000; break;
        case '15m': timeRange = 15 * 60 * 1000; break;
        case '60m': timeRange = 60 * 60 * 1000; break;
        default: timeRange = 5 * 60 * 1000;
    }
    const startTime = now - timeRange;
    
    // Initialize hidden systems set if not exists
    if (!state.hiddenCallRateSystems) state.hiddenCallRateSystems = new Set();
    if (state.hiddenCallRateTotal === undefined) state.hiddenCallRateTotal = false;
    
    // Calculate total line first (needed for yMax calculation)
    const systems = Object.keys(state.callRateHistory).filter(
        name => name && name !== 'undefined' && name !== 'Unknown' && name.trim() !== ''
    );
    const totalData = [];
    const allTimes = new Set();
    systems.forEach(sysName => {
        state.callRateHistory[sysName].forEach(p => {
            if (p.time >= startTime) allTimes.add(p.time);
        });
    });
    
    Array.from(allTimes).sort((a, b) => a - b).forEach(time => {
        let sum = 0;
        systems.forEach(sysName => {
            const point = state.callRateHistory[sysName].find(p => p.time === time);
            if (point) sum += point.count;
        });
        totalData.push({ time, count: sum });
    });
    
    // Calculate max Y value from data (include visible systems and total if shown)
    const yMin = 0;
    const yStep = 2;  // Always use intervals of 2
    let yMax = 10;  // Minimum range
    
    const visibleSystems = Object.keys(state.callRateHistory).filter(s => !state.hiddenCallRateSystems.has(s));
    const allData = visibleSystems.flatMap(s => state.callRateHistory[s] || []).filter(p => p.time >= startTime);
    
    // Also include total data if total line is visible
    const dataForScaling = allData.length > 0 ? allData.map(p => p.count) : [];
    if (!state.hiddenCallRateTotal && totalData.length > 0) {
        dataForScaling.push(...totalData.map(p => p.count));
    }
    
    if (dataForScaling.length > 0) {
        const maxCount = Math.max(...dataForScaling);
        // Round up to next even number (multiple of 2) with headroom
        const targetMax = maxCount + 1;
        yMax = Math.max(10, Math.ceil(targetMax / yStep) * yStep);
    }
    
    // Draw grid
    ctx.strokeStyle = cssVar('--border', '#2a2a4e');
    ctx.lineWidth = 1;
    
    // Horizontal grid lines
    for (let y = 0; y <= yMax; y += yStep) {
        const yPos = padding.top + chartHeight - (y / yMax * chartHeight);
        ctx.beginPath();
        ctx.moveTo(padding.left, yPos);
        ctx.lineTo(width - padding.right, yPos);
        ctx.stroke();
        
        // Y-axis labels
        ctx.fillStyle = cssVar('--text-secondary', '#a0a0a0');
        ctx.font = '11px sans-serif';
        ctx.textAlign = 'right';
        ctx.fillText(y.toString(), padding.left - 5, yPos + 4);
    }
    
    // Time labels
    ctx.fillStyle = cssVar('--text-secondary', '#a0a0a0');
    ctx.font = '11px sans-serif';
    ctx.textAlign = 'center';
    const timeLabels = currentCallChartPeriod === '5m' ? 5 : currentCallChartPeriod === '15m' ? 5 : 6;
    for (let i = 0; i <= timeLabels; i++) {
        const t = startTime + (timeRange * i / timeLabels);
        const x = padding.left + (chartWidth * i / timeLabels);
        const date = new Date(t);
        ctx.fillText(date.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'}), x, height - 10);
    }
    
    // Y-axis title
    ctx.save();
    ctx.translate(15, height / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.textAlign = 'center';
    ctx.fillStyle = cssVar('--text-secondary', '#a0a0a0');
    ctx.fillText('Active Calls', 0, 0);
    ctx.restore();
    
    // Draw data lines for each system + total
    // Filter out invalid system names (undefined, Unknown, empty)
    const legendContainer = document.getElementById('callChartLegend');
    if (legendContainer) {
        legendContainer.innerHTML = '';
    }
    
    const palette = getChartPalette();

    // Draw individual system lines as step charts (calls are discrete)
    systems.forEach((sysName, idx) => {
        const data = state.callRateHistory[sysName].filter(p => p.time >= startTime);
        const color = palette[idx % palette.length];
        const isHidden = state.hiddenCallRateSystems.has(sysName);
        
        // Draw line if not hidden and has data
        if (!isHidden && data.length >= 1) {
            ctx.strokeStyle = color;
            ctx.lineWidth = 2;
            ctx.beginPath();
            
            // Step-based rendering: calls remain constant until they change
            data.forEach((point, i) => {
                const x = padding.left + ((point.time - startTime) / timeRange * chartWidth);
                const y = padding.top + chartHeight - (point.count / yMax * chartHeight);
                
                if (i === 0) {
                    // Start at first point
                    ctx.moveTo(x, y);
                } else {
                    // Draw horizontal line from previous point, then vertical step to new value
                    const prevPoint = data[i - 1];
                    const prevX = padding.left + ((prevPoint.time - startTime) / timeRange * chartWidth);
                    const prevY = padding.top + chartHeight - (prevPoint.count / yMax * chartHeight);
                    
                    // Horizontal plateau at previous value
                    ctx.lineTo(x, prevY);
                    // Vertical step to new value
                    ctx.lineTo(x, y);
                }
                
                // Extend plateau to next point or end of chart
                if (i === data.length - 1) {
                    ctx.lineTo(width - padding.right, y);
                }
            });
            
            ctx.stroke();
        }
        
        // Add to legend (always show, even if hidden)
        if (legendContainer) {
            const item = document.createElement('div');
            item.className = 'legend-item' + (isHidden ? ' legend-hidden' : '');
            item.style.cursor = 'pointer';
            item.style.opacity = isHidden ? '0.4' : '1';
            item.innerHTML = `<div class="legend-color" style="background: ${color}"></div>${sysName}`;
            item.onclick = () => {
                if (state.hiddenCallRateSystems.has(sysName)) {
                    state.hiddenCallRateSystems.delete(sysName);
                } else {
                    state.hiddenCallRateSystems.add(sysName);
                }
                updateCallRateChart();
            };
            legendContainer.appendChild(item);
        }
    });
    
    // Draw total line as step chart (subtle, semi-transparent)
    if (!state.hiddenCallRateTotal && totalData.length >= 1) {
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.35)';
        ctx.lineWidth = 1.5;
        ctx.setLineDash([4, 4]);
        ctx.beginPath();
        
        totalData.forEach((point, i) => {
            const x = padding.left + ((point.time - startTime) / timeRange * chartWidth);
            const y = padding.top + chartHeight - (point.count / yMax * chartHeight);
            
            if (i === 0) {
                ctx.moveTo(x, y);
            } else {
                // Step-based: horizontal plateau then vertical step
                const prevPoint = totalData[i - 1];
                const prevX = padding.left + ((prevPoint.time - startTime) / timeRange * chartWidth);
                const prevY = padding.top + chartHeight - (prevPoint.count / yMax * chartHeight);
                
                ctx.lineTo(x, prevY);
                ctx.lineTo(x, y);
            }
            
            // Extend to end of chart on last point
            if (i === totalData.length - 1) {
                ctx.lineTo(width - padding.right, y);
            }
        });
        
        ctx.stroke();
        ctx.setLineDash([]);
    }
        
    // Add total to legend (always show, with toggle)
    if (legendContainer) {
        const item = document.createElement('div');
        item.className = 'legend-item' + (state.hiddenCallRateTotal ? ' legend-hidden' : '');
        item.style.cursor = 'pointer';
        item.style.opacity = state.hiddenCallRateTotal ? '0.4' : '1';
        item.innerHTML = `<div class="legend-color" style="background: rgba(255,255,255,0.35); border: 1px dashed rgba(255,255,255,0.5)"></div>Total`;
        item.onclick = () => {
            state.hiddenCallRateTotal = !state.hiddenCallRateTotal;
            updateCallRateChart();
        };
        legendContainer.appendChild(item);
    }
}

// Admin functions
function refreshLoginHistory() {
    fetch(`${BASE_PATH}api/admin/login-history`, {
        credentials: 'include'
    })
    .then(response => {
        if (!response.ok) throw new Error('Failed to load login history');
        return response.json();
    })
    .then(data => {
        const tbody = document.getElementById('loginHistoryTable');
        if (!tbody) return;
        
        if (!data || data.length === 0) {
            tbody.innerHTML = '<tr><td colspan="5">No login history available</td></tr>';
            return;
        }
        
        tbody.innerHTML = '';
        data.reverse().forEach(entry => {
            const tr = document.createElement('tr');
            const date = new Date(entry.timestamp * 1000);
            const timeStr = date.toLocaleString();
            
            const successBadge = entry.success 
                ? '<span class="badge badge-success">Success</span>'
                : '<span class="badge badge-error">Failed</span>';
            
            const levelBadge = entry.access_level === 'admin'
                ? '<span class="badge badge-admin">Admin</span>'
                : entry.access_level === 'info'
                ? '<span class="badge badge-info">Info</span>'
                : '<span class="badge badge-error">Failed</span>';
            
            tr.innerHTML = `
                <td>${timeStr}</td>
                <td>${entry.username || 'none'}</td>
                <td>${entry.client_ip}</td>
                <td>${levelBadge}</td>
                <td>${successBadge}</td>
            `;
            tbody.appendChild(tr);
        });
    })
    .catch(error => {
        console.error('Error loading login history:', error);
        const tbody = document.getElementById('loginHistoryTable');
        if (tbody) {
            tbody.innerHTML = '<tr><td colspan="5" style="color: red;">Error loading login history</td></tr>';
        }
    });
}

let originalConfig = '';
let configPath = './config.json';
let hasUnsavedChanges = false;

function showAdminSection(section) {
    // Check for unsaved changes
    if (hasUnsavedChanges) {
        if (!confirm('You have unsaved changes in the Advanced Editor. These changes will be lost if you navigate away. Continue?')) {
            return;
        }
        hasUnsavedChanges = false;
    }
    
    // Update menu items
    document.querySelectorAll('.admin-menu-item').forEach(item => {
        item.classList.toggle('active', item.dataset.section === section);
    });
    
    // Update sections - explicitly set display style
    document.querySelectorAll('.admin-section').forEach(sec => {
        if (sec.id === `admin-${section}`) {
            sec.style.display = 'block';
            sec.classList.add('active');
        } else {
            sec.style.display = 'none';
            sec.classList.remove('active');
        }
    });
    
    // Load data for specific sections
    if (section === 'editor') {
        loadConfig();
        // Ensure line numbers update after a brief delay for rendering
        setTimeout(() => {
            updateLineNumbers();
            updateHighlight();
        }, 50);
    } else if (section === 'history') {
        refreshLoginHistory();
    }
}

function loadConfig() {
    const editor = document.getElementById('configEditor');
    const highlight = document.getElementById('configHighlight');
    const status = document.getElementById('configStatus');
    const pathDisplay = document.getElementById('configFilePath');
    if (!editor) return;
    
    editor.value = 'Loading config...';
    if (highlight) highlight.innerHTML = '';
    if (status) status.textContent = '';
    if (pathDisplay) pathDisplay.textContent = '';
    clearValidation();
    hasUnsavedChanges = false;
    updateLineNumbers();
    
    fetch(`${BASE_PATH}api/admin/config`, {
        credentials: 'include'
    })
    .then(response => {
        if (!response.ok) throw new Error('Failed to load config');
        return response.json();
    })
    .then(data => {
        // Parse and re-stringify with 4-space indent for readability
        try {
            const parsed = JSON.parse(data.content);
            const formatted = JSON.stringify(parsed, null, 4);
            originalConfig = formatted;
            configPath = data.path || './config.json';
            editor.value = formatted;
        } catch (e) {
            // If parsing fails, use raw content
            originalConfig = data.content;
            configPath = data.path || './config.json';
            editor.value = originalConfig;
        }
        if (pathDisplay) pathDisplay.textContent = configPath;
        updateLineNumbers();
        updateHighlight();
        validateJSON();
    })
    .catch(error => {
        console.error('Error loading config:', error);
        editor.value = 'Error loading config: ' + error.message;
        updateLineNumbers();
    });
}

function reloadConfig() {
    if (hasUnsavedChanges) {
        if (!confirm('You have unsaved changes. Reloading will discard them. Continue?')) {
            return;
        }
    }
    loadConfig();
}

function validateJSON() {
    const editor = document.getElementById('configEditor');
    const validation = document.getElementById('configValidation');
    const status = document.getElementById('configStatus');
    const lineNumbersDiv = document.getElementById('lineNumbers');
    
    if (!editor || !validation) return false;
    
    const content = editor.value;
    
    // Clear any previous error highlighting
    if (lineNumbersDiv) {
        const lineCount = content.split('\n').length;
        lineNumbersDiv.innerHTML = Array.from({length: lineCount}, (_, i) => i + 1).join('\n');
    }
    
    try {
        JSON.parse(content);
        validation.style.display = 'none';
        if (status) {
            status.innerHTML = '<span style="color: var(--accent-green);">✓ Valid JSON</span>';
        }
        return true;
    } catch (e) {
        validation.style.display = 'block';
        validation.style.background = 'rgba(231, 93, 87, 0.1)';
        validation.style.border = '1px solid var(--accent)';
        validation.style.color = 'var(--accent)';
        
        // Clear any previous highlighting
        if (lineNumbersDiv) {
            const lineCount = content.split('\n').length;
            lineNumbersDiv.innerHTML = Array.from({length: lineCount}, (_, i) => i + 1).join('\n');
        }
        
        // Show clear error message - native JSON.parse() doesn't provide reliable line numbers
        const errorMsg = '<strong style="font-size: 1em;">⚠ JSON SYNTAX ERROR:</strong> ' + escapeHtml(e.message);
        validation.innerHTML = errorMsg;
        
        if (status) {
            status.innerHTML = '<span style="color: var(--accent);">✗ Invalid JSON</span>';
        }
        return false;
    }
}

function clearValidation() {
    const validation = document.getElementById('configValidation');
    if (validation) {
        validation.style.display = 'none';
    }
    // Regenerate line numbers without highlighting
    updateLineNumbers();
}

function saveConfig() {
    const editor = document.getElementById('configEditor');
    const status = document.getElementById('configStatus');
    
    if (!editor) return;
    
    // Validate JSON before saving
    if (!validateJSON()) {
        alert('Cannot save: Configuration contains invalid JSON. Please fix the syntax errors first.');
        return;
    }
    
    const content = editor.value;
    
    // Check if modified
    if (content === originalConfig) {
        alert('No changes detected.');
        return;
    }
    
    if (!confirm('Are you sure you want to save this configuration?\n\nA backup will be created automatically.\nYou will need to restart trunk-recorder for changes to take effect.')) {
        return;
    }
    
    if (status) status.textContent = 'Saving...';
    
    fetch(`${BASE_PATH}api/admin/save-config`, {
        method: 'POST',
        credentials: 'include',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({
            content: content,
            path: configPath
        })
    })
    .then(response => {
        if (!response.ok) {
            return response.json().then(err => {
                throw new Error(err.error || 'Failed to save config');
            });
        }
        return response.json();
    })
    .then(data => {
        originalConfig = content;
        hasUnsavedChanges = false;
        if (status) {
            status.innerHTML = '<span style="color: var(--accent-green);">✓ Saved successfully</span>';
            setTimeout(() => {
                status.innerHTML = '<span style="color: var(--accent-green);">✓ Valid JSON</span>';
            }, 3000);
        }
        alert(`Configuration saved successfully!\n\nBackup created: ${data.backup}\n\nRestart trunk-recorder to apply changes.`);
    })
    .catch(error => {
        console.error('Error saving config:', error);
        if (status) {
            status.innerHTML = '<span style="color: var(--accent);">✗ Save failed</span>';
        }
        alert('Error saving configuration: ' + error.message);
    });
}

// JSON syntax highlighter
function highlightJSON(text) {
    if (!text) return '';
    
    // Single-pass tokenizer: match all JSON tokens at once
    // Pattern matches: strings | numbers | booleans | null | structural chars
    const tokenPattern = /"(?:[^"\\]|\\.)*"|(?:\b-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\b|\btrue\b|\bfalse\b|\bnull\b|[{}[\]:,]/g;
    
    let result = '';
    let lastIndex = 0;
    let match;
    let nextChar;
    
    while ((match = tokenPattern.exec(text)) !== null) {
        // Add any text between last match and this one (whitespace, etc)
        result += escapeHtml(text.substring(lastIndex, match.index));
        
        const token = match[0];
        
        if (token.startsWith('"')) {
            // It's a string - check if it's a key (followed by colon) or a value
            nextChar = text.substring(match.index + token.length).match(/^\s*:/);
            if (nextChar) {
                result += `<span class="json-key">${escapeHtml(token)}</span>`;
            } else {
                result += `<span class="json-string">${escapeHtml(token)}</span>`;
            }
        } else if (/^-?\d/.test(token)) {
            // Number
            result += `<span class="json-number">${token}</span>`;
        } else if (token === 'true' || token === 'false') {
            // Boolean
            result += `<span class="json-boolean">${token}</span>`;
        } else if (token === 'null') {
            // Null
            result += `<span class="json-null">${token}</span>`;
        } else if (token === '{' || token === '}') {
            // Braces
            result += `<span class="json-brace">${token}</span>`;
        } else if (token === '[' || token === ']') {
            // Brackets
            result += `<span class="json-bracket">${token}</span>`;
        } else if (token === ':') {
            // Colon
            result += `<span class="json-colon">${token}</span>`;
        } else if (token === ',') {
            // Comma
            result += `<span class="json-comma">${token}</span>`;
        }
        
        lastIndex = match.index + token.length;
    }
    
    // Add any remaining text
    result += escapeHtml(text.substring(lastIndex));
    
    return result;
}

function updateHighlight() {
    const editor = document.getElementById('configEditor');
    const highlight = document.getElementById('configHighlight');
    if (!highlight || !editor) return;
    highlight.innerHTML = highlightJSON(editor.value) + '\n';
}

function updateLineNumbers() {
    const editor = document.getElementById('configEditor');
    const lineNumbersDiv = document.getElementById('lineNumbers');
    if (!editor || !lineNumbersDiv) return;
    
    const lines = editor.value.split('\n');
    const lineCount = lines.length;
    lineNumbersDiv.innerHTML = Array.from({length: lineCount}, (_, i) => i + 1).join('\n');
    lineNumbersDiv.scrollTop = editor.scrollTop;
}

// Add live validation and change tracking as user types
if (document.getElementById('configEditor')) {
    const editor = document.getElementById('configEditor');
    const highlight = document.getElementById('configHighlight');
    
    editor.addEventListener('input', function() {
        // Track changes
        hasUnsavedChanges = (this.value !== originalConfig);
        updateLineNumbers();
        updateHighlight();
        
        // Debounce validation
        clearTimeout(this.validateTimeout);
        this.validateTimeout = setTimeout(() => validateJSON(), 500);
    });
    
    // Sync line numbers scroll with editor scroll
    editor.addEventListener('scroll', function() {
        const lineNumbersDiv = document.getElementById('lineNumbers');
        if (lineNumbersDiv) {
            lineNumbersDiv.scrollTop = this.scrollTop;
        }
        if (highlight) {
            highlight.scrollTop = this.scrollTop;
            highlight.scrollLeft = this.scrollLeft;
        }
    });
    
    // Initialize line numbers and highlighting
    updateLineNumbers();
    updateHighlight();
}

function restartTrunkRecorder() {
    if (!confirm('Are you sure you want to restart trunk-recorder? This will interrupt all active recordings.')) {
        return;
    }
    
    fetch(`${BASE_PATH}api/admin/restart`, {
        method: 'POST',
        credentials: 'include'
    })
    .then(response => {
        if (!response.ok) throw new Error('Failed to initiate restart');
        return response.json();
    })
    .then(data => {
        alert('Restart initiated. The application will restart shortly.');
    })
    .catch(error => {
        console.error('Error restarting:', error);
        alert('Error initiating restart: ' + error.message);
    });
}

// Tab switching
// Initialize
document.addEventListener('DOMContentLoaded', () => {
    initThemePicker();
    initRecentRetentionPicker();
    // Default theme unless overridden by config
    applyTheme('nostromo');

    initChart();
    initWrapToggle();
    connect();
    
    // Detect and display auth level
    updateAuthDisplay();
    
    // Fetch initial data
    fetch(`${BASE_PATH}api/status`, {
        credentials: 'include'
    })
        .then(r => r.json())
        .then(data => {
            if (data.recorders) state.recorders = data.recorders;
            if (data.calls) state.calls = data.calls;
            if (data.systems) state.systems = data.systems;
            if (data.devices) state.devices = data.devices;
            if (data.rateHistory) state.rateHistory = data.rateHistory;
            if (data.callRateHistory) state.callRateHistory = data.callRateHistory;
            if (data.trunkMessages) state.trunkMessages = data.trunkMessages;
            if (data.unitAffiliations) {
                state.unitAffiliations = data.unitAffiliations;
            }
            if (data.consoleLogs) {
                state.consoleLogs = data.consoleLogs;
                hasScrolledConsole = false;
                renderConsole();
                // Double-check scroll after a delay to ensure DOM is fully rendered
                setTimeout(() => {
                    const container = document.getElementById('consoleOutput');
                    if (container) container.scrollTop = container.scrollHeight;
                }, 100);
            }
            if (data.config) {
                state.config = data.config;
                if (state.config && state.config.theme) {
                    applyTheme(state.config.theme);
                }
                // Apply console limit from backend config
                if (state.config && state.config.console_max_lines) {
                    state.consoleMaxLines = state.config.console_max_lines;
                }
            }
            
            // Initialize recent calls from cached history (newest first)
            if (data.callHistory && Array.isArray(data.callHistory)) {
                state.recentCalls = data.callHistory.reverse().map(c => {
                    const endedAtMs = (c.stop_time) ? (Number(c.stop_time) * 1000) : Date.now();
                    return {
                        key: callKey(c) || `${c.talkgroup || 0}-${c.start_time || 0}`,
                        endedAtMs: endedAtMs,
                        call: normalizeCallSummary(c, null),
                        json: c
                    };
                });
                trimRecentCalls();
            }
            
            updateRecordersTable();
            // Seed last-seen map for selection/details
            state.calls.forEach(c => {
                const k = callKey(c);
                if (k) {
                    state.lastActiveByKey.set(k, c);
                    state.lastActiveSeenAt.set(k, Date.now());
                }
            });
            updateCallsTable();
            updateSystemTabs();
            updateDevicesTiles();
            updateStats();
            updateChart();
            updateCallRateChart();
        })
        .catch(console.error);
    
    // Redraw chart on resize
    window.addEventListener('resize', () => {
        if (document.querySelector('#panel-status.active')) {
            updateChart();
            updateCallRateChart();
        }
    });

    // Combined calls table row selection (both active and recent)
    const allCallsTbody = document.querySelector('#allCallsTable tbody');
    if (allCallsTbody) {
        allCallsTbody.addEventListener('click', (ev) => {
            const tr = ev.target.closest('tr');
            if (!tr) return;
            
            // Check if it's an active or recent call
            const activeKey = tr.getAttribute('data-active-key');
            const recentKey = tr.getAttribute('data-recent-key');
            
            if (activeKey) {
                state.selectedActiveCallKey = activeKey;
                state.selectedRecentCallKey = null;
            } else if (recentKey) {
                state.selectedRecentCallKey = recentKey;
                state.selectedActiveCallKey = null;
            }
            
            updateCallsTable();
            updateDetailsPane();
        });
    }

    // Periodically prune STOPPED calls into Recent
    setInterval(() => {
        pruneStoppedCalls();
        updateCallsTable();
    }, 500);
    
    // Update voice channels periodically
    setInterval(() => {
        updateVoiceChannels();
    }, 1000);
    
    // Load admin data when admin tab is shown
    window.showMainTab = showMainTab;
});

// Omnitrunker Functions

function initOmnitrunker() {
    const systemFilter = document.getElementById('omniSystemFilter');
    if (systemFilter && state.systems.length) {
        const currentValue = systemFilter.value;
        systemFilter.innerHTML = '<option value="">All Systems</option>';
        state.systems.forEach(sys => {
            const opt = document.createElement('option');
            const sysName = sys.unique_sys_name || sys.short_name || sys.sys_name;
            opt.value = sysName;
            opt.textContent = sysName;
            systemFilter.appendChild(opt);
        });
        systemFilter.value = currentValue;
    }
    
    // Set buffer size selector
    const bufferSel = document.getElementById('omniBufferSize');
    if (bufferSel) {
        bufferSel.value = state.omniBufferSize.toString();
    }
    
    // Load filter preferences
    const autoScrollCb = document.getElementById('omniAutoScroll');
    if (autoScrollCb) {
        state.omniAutoScroll = autoScrollCb.checked;
        autoScrollCb.addEventListener('change', () => {
            state.omniAutoScroll = autoScrollCb.checked;
        });
    }
    
    setupOmniResize();
}

function setupOmniResize() {
    const handle = document.getElementById('omniResizeHandle');
    const channelsPanel = document.querySelector('.omni-channels');
    
    if (!handle || !channelsPanel) return;
    
    const MIN_PERCENT = 20;
    const MAX_PERCENT = 80;
    let isResizing = false;
    
    handle.addEventListener('mousedown', (e) => {
        isResizing = true;
        document.body.style.cursor = 'ns-resize';
        document.body.style.userSelect = 'none';
        e.preventDefault();
    });
    
    document.addEventListener('mousemove', (e) => {
        if (!isResizing) return;
        
        const container = document.querySelector('.omni-container');
        if (!container) return;
        
        const rect = container.getBoundingClientRect();
        const percent = Math.max(MIN_PERCENT, Math.min(MAX_PERCENT, 
            ((e.clientY - rect.top) / rect.height) * 100));
        
        channelsPanel.style.flex = `0 0 ${percent}%`;
        channelsPanel.style.maxHeight = 'none';
    });
    
    document.addEventListener('mouseup', () => {
        if (isResizing) {
            isResizing = false;
            document.body.style.cursor = '';
            document.body.style.userSelect = '';
        }
    });
}

function filterOmniSystem() {
    const sel = document.getElementById('omniSystemFilter');
    if (sel) {
        state.omniSystemFilter = sel.value;
        updateVoiceChannels();
        updateOmniMessages();
    }
}

function filterOmniMessages() {
    const sel = document.getElementById('omniMsgTypeFilter');
    if (sel) {
        state.omniMsgTypeFilter = sel.value;
        updateOmniMessages();
    }
}

function clearOmniMessages() {
    state.trunkMessages = [];
    updateOmniMessages();
}

function changeOmniBuffer() {
    const sel = document.getElementById('omniBufferSize');
    if (sel) {
        state.omniBufferSize = parseInt(sel.value);
        // Trim to new size if needed
        if (state.trunkMessages.length > state.omniBufferSize) {
            state.trunkMessages = state.trunkMessages.slice(-state.omniBufferSize);
        }
        updateOmniMessages();
    }
}

function updateVoiceChannels() {
    const tbody = document.getElementById('voiceChannelsBody');
    if (!tbody) return;
    
    // Filter active calls by system if needed - use call_state_type from backend
    let activeCalls = state.calls.filter(c => c.call_state_type && c.call_state_type !== 'MONITORING');
    if (state.omniSystemFilter) {
        // Use unique_sys_name from backend
        activeCalls = activeCalls.filter(c => c.unique_sys_name === state.omniSystemFilter);
    }
    
    if (activeCalls.length === 0) {
        tbody.innerHTML = '<tr><td colspan="9" class="empty-state">No active voice channels</td></tr>';
        return;
    }
    
    // Sort by frequency
    activeCalls.sort((a, b) => (a.freq || 0) - (b.freq || 0));
    
    tbody.innerHTML = activeCalls.map(call => {
        const sysName = call.sys_name || `System ${call.sys_num}`;
        const freq = call.freq ? (call.freq / 1e6).toFixed(4) : '---';
        const tg = call.talkgroup || '---';
        const tgAlpha = call.talkgroup_alpha_tag || '';
        const source = call.unit || '';
        const srcAlias = call.unit_alpha_tag || '';
        const elapsed = call.elapsed ? `${call.elapsed}s` : '';
        // Consolidate mode and slot: FDMA, TDMA-0, TDMA-1
        const mode = call.phase2_tdma ? `TDMA-${call.tdma_slot}` : 'FDMA';
        const encrypted = call.encrypted ? '<span class="omni-encrypted-badge">ENCRYPTED</span>' : '';
        
        return `
            <tr>
                <td class="omni-msg-system">${escapeHtml(sysName)}</td>
                <td class="omni-channel-freq">${freq}</td>
                <td class="omni-channel-tg">${tg}</td>
                <td class="omni-channel-alpha">${escapeHtml(tgAlpha)}</td>
                <td class="omni-channel-source">${source}</td>
                <td class="omni-channel-alias">${escapeHtml(srcAlias)}</td>
                <td class="omni-channel-elapsed">${elapsed}</td>
                <td class="omni-channel-mode">${mode}</td>
                <td class="omni-channel-encrypted">${encrypted}</td>
            </tr>
        `;
    }).join('');
}

function updateOmniMessages() {
    const tbody = document.getElementById('omniMessagesBody');
    if (!tbody) return;
    
    // Filter messages
    let messages = state.trunkMessages;
    
    if (state.omniSystemFilter) {
        messages = messages.filter(m => m.unique_sys_name === state.omniSystemFilter);
    }
    
    if (state.omniMsgTypeFilter) {
        messages = messages.filter(m => m.msg_type === state.omniMsgTypeFilter);
    }
    
    // Show most recent messages up to buffer size, reversed (newest first)
    const recent = messages.slice(-state.omniBufferSize).reverse();
    
    tbody.innerHTML = recent.map(msg => {
        const time = new Date(msg.timestamp * 1000).toLocaleTimeString();
        const msgType = msg.msg_type || 'UNKNOWN';
        const siteId = msg.site_id || '';
        const unit = msg.unit || '';
        const unitAlias = msg.unit_alias || '';
        const tg = msg.talkgroup || '';
        const tgAlpha = msg.tg_alpha || '';
        
        return `
            <tr>
                <td class="omni-msg-time">${time}</td>
                <td class="omni-msg-system">${escapeHtml(msg.sys_name || '')}</td>
                <td class="omni-msg-site">${siteId}</td>
                <td class="omni-msg-unit">${unit}</td>
                <td class="omni-msg-alias">${escapeHtml(unitAlias)}</td>
                <td><span class="omni-msg-type omni-msg-type-${msgType}">${msgType}</span></td>
                <td class="omni-msg-tg">${tg}</td>
                <td class="omni-msg-tg-alpha">${escapeHtml(tgAlpha)}</td>
            </tr>
        `;
    }).join('');
    
    // With reverse chrono (newest first), scroll to top to see new messages
    if (state.omniAutoScroll) {
        const container = document.querySelector('.omni-messages-container');
        if (container) {
            container.scrollTop = 0;
        }
    }
}

// ========================================
// Affiliations Tab
// ========================================

const affiliationState = {
    units: [],
    talkgroups: [],
    currentView: 'talkgroups',
    sortColumn: 'id',
    sortDirection: 'asc',
    searchTerm: '',
    filterBogons: true
};

function toggleApiInfo() {
        // Setup copy button with clipboard API and feedback
        const copyBtn = document.getElementById('copyGraphStreamUrl');
        if (copyBtn) {
            copyBtn.onclick = function() {
                const url = document.getElementById('graphStreamUrl').textContent;
                if (navigator.clipboard) {
                    navigator.clipboard.writeText(url).then(() => {
                        copyBtn.textContent = 'Copied!';
                        setTimeout(() => { copyBtn.textContent = 'Copy'; }, 1200);
                    });
                } else {
                    // Fallback for older browsers
                    const tempInput = document.createElement('input');
                    tempInput.value = url;
                    document.body.appendChild(tempInput);
                    tempInput.select();
                    document.execCommand('copy');
                    document.body.removeChild(tempInput);
                    copyBtn.textContent = 'Copied!';
                    setTimeout(() => { copyBtn.textContent = 'Copy'; }, 1200);
                }
            };
        }
    const infoBox = document.getElementById('apiInfoBox');
    if (infoBox) {
        const isHidden = infoBox.style.display === 'none';
        infoBox.style.display = isHidden ? 'block' : 'none';
        
        // Populate URL when showing
        if (isHidden) {
            const urlElement = document.getElementById('graphStreamUrl');
            if (urlElement) {
                const port = window.location.port ? `:${window.location.port}` : '';
                const basePath = BASE_PATH || '';
                const path = basePath.endsWith('/') ? `${basePath}graph-stream` : `${basePath}/graph-stream`;
                const url = `${window.location.protocol}//${window.location.hostname}${port}${path}`;
                urlElement.textContent = url;
            }
        }
    }
}

function showAffiliationView(view) {
    affiliationState.currentView = view;
    
    // Update tabs
    document.querySelectorAll('.affiliation-tab').forEach(tab => {
        tab.classList.toggle('active', tab.dataset.view === view);
    });
    
    // Update views
    document.querySelectorAll('.affiliation-view').forEach(v => {
        v.classList.toggle('active', v.id === `view-${view}`);
    });
    
    renderAffiliationTable();
}

async function loadAffiliations() {
    console.log('[Affiliations] loadAffiliations called');
    
    // Update loading message to show we're actually trying
    // Show loading overlay or spinner if desired, but do not clear table
    
    try {
        console.log('[Affiliations] Fetching data from /api/affiliations');
        const response = await fetch(`${BASE_PATH}api/affiliations`, {
            credentials: 'same-origin',
            headers: {
                'Accept': 'application/json'
            }
        });
        console.log('[Affiliations] Response status:', response.status, response.statusText);
        
        if (!response.ok) {
            console.error('[Affiliations] Failed to fetch:', response.status, response.statusText);
            const errorText = await response.text();
            console.error('[Affiliations] Error body:', errorText);
            // Do not clear table while fetching
            return;
        }
        
        const data = await response.json();
        console.log('[Affiliations] Received data:', { units: data.units?.length || 0, talkgroups: data.talkgroups?.length || 0 });
        affiliationState.units = data.units || [];
        affiliationState.talkgroups = data.talkgroups || [];
        
        renderAffiliationTable();
    } catch (e) {
        console.error('[Affiliations] Exception caught:', e);
        console.error('[Affiliations] Stack:', e.stack);
        // Do not clear table on error
    }
}

function formatTimestamp(timestamp) {
    if (!timestamp) return 'Never';
    
    const now = Math.floor(Date.now() / 1000);
    const diff = now - timestamp;
    
    if (diff < 60) return 'Just now';
    if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
    if (diff < 86400) return `${Math.floor(diff / 3600)}h ago`;
    return `${Math.floor(diff / 86400)}d ago`;
}

function renderStatusBadge(item, isUnit) {
    const now = Math.floor(Date.now() / 1000);
    const idleThreshold = 12 * 3600;
    const isIdle = (now - item.last_active) > idleThreshold;
    
    if (isUnit && !item.registered) {
        return '<span class="status-badge deregistered">⚪ Deregistered</span>';
    } else if (isIdle) {
        return '<span class="status-badge idle">⚫ Idle</span>';
    } else {
        return '<span class="status-badge active">🟢 Active</span>';
    }
}

function renderEncryptionBadge(item) {
    if (item.ever_encrypted) {
        return '<span class="status-badge encrypted">🔴 Encrypted</span>';
    }
    return '<span style="color: var(--text-secondary);">—</span>';
}

function renderStatusBadges(item, isUnit) {
    const badges = [];
    
    if (item.ever_encrypted) {
        badges.push('<span class="status-badge encrypted">🔴 Encrypted</span>');
    }
    
    if (isUnit && !item.registered) {
        badges.push('<span class="status-badge deregistered">⚪ Deregistered</span>');
    } else if (item.is_idle) {
        badges.push('<span class="status-badge idle">⚫ Idle</span>');
    } else {
        badges.push('<span class="status-badge active">● Active</span>');
    }
    
    return badges.join(' ');
}

function renderAssociatedItems(counts, isUnit) {
    const entries = Object.entries(counts);
    if (entries.length === 0) return '<span style="color: var(--text-secondary);">None</span>';
    
    // Sort by transmission count descending
    entries.sort((a, b) => b[1] - a[1]);
    
    const itemClass = isUnit ? 'tg-chip' : 'unit-chip';
    const clickHandler = isUnit ? 'jumpToTalkgroup' : 'jumpToUnit';
    
    return `<div class="${isUnit ? 'tg-list' : 'unit-list'}">` +
        entries.map(([id, count]) => 
            `<span class="${itemClass}" onclick="${clickHandler}('${id}')">
                ${id} <span class="transmission-badge">${count}</span>
            </span>`
        ).join('') +
        '</div>';
}

function sortAffiliationTable(view, column) {
    if (affiliationState.sortColumn === column) {
        affiliationState.sortDirection = affiliationState.sortDirection === 'asc' ? 'desc' : 'asc';
    } else {
        affiliationState.sortColumn = column;
        affiliationState.sortDirection = 'desc';
    }
    renderAffiliationTable();
}

function filterAndSortData(items) {
    let filtered = items;
    // Apply bogon filter (unit/tg 0, -1)
    const filterBogons = document.getElementById('filterBogonsCheckbox')?.checked ?? true;
    if (filterBogons) {
        filtered = filtered.filter(item => item.id !== 0 && item.id !== -1);
        // Deep filter associations as well
        filtered = filtered.map(item => {
            // Clone to avoid mutating original
            const newItem = { ...item };
            if (newItem.talkgroup_transmission_counts) {
                newItem.talkgroup_transmission_counts = Object.fromEntries(
                    Object.entries(newItem.talkgroup_transmission_counts)
                        .filter(([tgid]) => tgid !== '0' && tgid !== '-1')
                );
            }
            if (newItem.unit_transmission_counts) {
                newItem.unit_transmission_counts = Object.fromEntries(
                    Object.entries(newItem.unit_transmission_counts)
                        .filter(([uid]) => uid !== '0' && uid !== '-1')
                );
            }
            return newItem;
        });
    }
    
    // Apply search filter
    if (affiliationState.searchTerm) {
        const term = affiliationState.searchTerm.toLowerCase();
        filtered = filtered.map(item => {
            let newItem = { ...item };
            let match = String(item.id).includes(term) ||
                (item.alias && item.alias.toLowerCase().includes(term)) ||
                String(item.wacn).includes(term) ||
                String(item.sysid).includes(term);

            // If top-level matches, show all associations
            if (match) {
                // Do not filter associations
                return newItem;
            }

            // Otherwise, filter associations
            let hasAssociations = false;
            if (newItem.talkgroup_transmission_counts) {
                newItem.talkgroup_transmission_counts = Object.fromEntries(
                    Object.entries(newItem.talkgroup_transmission_counts)
                        .filter(([tgid]) => tgid.toLowerCase().includes(term))
                );
                if (Object.keys(newItem.talkgroup_transmission_counts).length > 0) hasAssociations = true;
            }
            if (newItem.unit_transmission_counts) {
                newItem.unit_transmission_counts = Object.fromEntries(
                    Object.entries(newItem.unit_transmission_counts)
                        .filter(([uid]) => uid.toLowerCase().includes(term))
                );
                if (Object.keys(newItem.unit_transmission_counts).length > 0) hasAssociations = true;
            }
            return hasAssociations ? newItem : null;
        }).filter(Boolean);
    }
    
    // Apply sorting
    filtered.sort((a, b) => {
        const col = affiliationState.sortColumn;
        let aVal = a[col];
        let bVal = b[col];
        
        if (col === 'alias') {
            aVal = (aVal || '').toLowerCase();
            bVal = (bVal || '').toLowerCase();
        }
        
        if (aVal < bVal) return affiliationState.sortDirection === 'asc' ? -1 : 1;
        if (aVal > bVal) return affiliationState.sortDirection === 'asc' ? 1 : -1;
        return 0;
    });
    
    return filtered;
}

function renderAffiliationTable() {
    const view = affiliationState.currentView;
    const isUnits = view === 'units';
    const data = isUnits ? affiliationState.units : affiliationState.talkgroups;
    
    const filtered = filterAndSortData(data);
    
    const tbody = document.getElementById(isUnits ? 'unitTableBody' : 'talkgroupTableBody');
    // Always fully rebuild the table to avoid orphaned/stale rows
    tbody.innerHTML = filtered.map((item, idx) => {
        const associatedCounts = isUnits ? item.talkgroup_transmission_counts : item.unit_transmission_counts;
        const itemKey = `${item.wacn}:${item.sysid}:${item.id}`;
        const rowId = `aff-row-${itemKey.replace(/:/g, '-')}`;
        const detailsId = `aff-details-${itemKey.replace(/:/g, '-')}`;
        const hasAssociations = associatedCounts && Object.keys(associatedCounts).length > 0;
        let html = '';
        // Always allow toggling, but auto-collapse if no associations
        html += '<tr id="' + rowId + '" class="affiliation-main-row" style="cursor: pointer; font-size: 15px;" onclick="toggleAffiliationDetails(\'' + detailsId + '\')">';
        html += '<td><strong style="font-size: 1.25em;">' + escapeHtml(item.id) + '</strong></td>';
        html += '<td><code style="color: var(--text-secondary); font-size: 12px;">' + escapeHtml(item.wacn) + '</code></td>';
        html += '<td><code style="color: var(--text-secondary); font-size: 12px;">' + escapeHtml(item.sysid) + '</code></td>';
        html += '<td>' + (item.alias ? '<span style="font-size: 1.15em; font-weight: 600;">' + escapeHtml(item.alias) + '</span>' : '<span style="color: var(--text-secondary);">—</span>') + '</td>';
        html += '<td>' + renderStatusBadge(item, isUnits) + '</td>';
        html += '<td>' + renderEncryptionBadge(item) + '</td>';
        html += '<td>' + formatTimestamp(item.last_active) + '</td>';
        html += '<td><strong>' + escapeHtml(item.transmission_count) + '</strong></td>';
        html += '</tr>';
        // Details row: auto-collapse if no associations
        html += '<tr id="' + detailsId + '" class="affiliation-details-row" style="display:' + (hasAssociations ? 'table-row' : 'none') + ';">';
        html += '<td colspan="8" style="padding: 0; background: var(--bg-tertiary);">';
        html += '<div style="padding: 12px 16px; border-top: 1px solid var(--border);">';
        html += '<div style="font-size: 11px; color: var(--text-secondary); margin-bottom: 8px; text-transform: uppercase; font-weight: 600;">';
        html += 'Associated ' + (isUnits ? 'Talkgroups' : 'Units') + ' (' + Object.keys(associatedCounts).length + ')';
        html += '</div>';
        html += '<div style="display: flex; flex-wrap: wrap; gap: 6px;">';
        html += renderAssociatedItemsCompact(associatedCounts, isUnits, item);
        html += '</div>';
        html += '</div>';
        html += '</td>';
        html += '</tr>';
        return html;
    }).join('');
    requestAnimationFrame(() => {
        if (scrollContainer) scrollContainer.scrollTop = savedScrollTop;
    });
    return;
}

function toggleAffiliationDetails(detailsId) {
    const row = document.getElementById(detailsId);
    if (row) {
        const isVisible = row.style.display !== 'none';
        row.style.display = isVisible ? 'none' : 'table-row';
    }
}

function renderAssociatedItemsCompact(counts, isUnits, parentItem) {
    if (!counts || Object.keys(counts).length === 0) {
        return '<span style="color: var(--text-secondary); font-size: 12px;">None</span>';
    }
    
    // Sort by transmission count descending
    const sorted = Object.entries(counts).sort((a, b) => b[1] - a[1]);
    
    return sorted.map(([id, count]) => {
        // Get the associated item's state to determine status and alias
        const associatedItem = isUnits ? 
            affiliationState.talkgroups.find(tg => tg.id == id && tg.wacn === parentItem.wacn && tg.sysid === parentItem.sysid) :
            affiliationState.units.find(u => u.id == id && u.wacn === parentItem.wacn && u.sysid === parentItem.sysid);
        
        const statusBadge = associatedItem ? getStatusBadge(associatedItem, !isUnits) : '';
        const clickHandler = isUnits ? 
            `jumpToTalkgroup(${id}, ${parentItem.wacn}, ${parentItem.sysid})` : 
            `jumpToUnit(${id}, ${parentItem.wacn}, ${parentItem.sysid})`;
        
        // Display alias if available, otherwise just the ID
        const displayText = associatedItem && associatedItem.alias ? 
            associatedItem.alias : 
            String(id);
        
        return `
            <div style="display: inline-flex; align-items: center; gap: 4px; padding: 4px 8px; 
                        background: var(--bg-secondary); border: 1px solid var(--border); 
                        border-radius: 4px; font-size: 12px; cursor: pointer; transition: all 0.2s;"
                 onclick="event.stopPropagation(); ${clickHandler};" 
                 onmouseover="this.style.background='var(--bg-hover)'" 
                 onmouseout="this.style.background='var(--bg-secondary)'"
                 title="${associatedItem && associatedItem.alias ? `${id}: ${associatedItem.alias}` : id}">
                ${statusBadge}
                <strong style="white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 200px;">${escapeHtml(displayText)}</strong>
                <span style="color: var(--text-secondary); margin-left: 2px;">(${count})</span>
            </div>
        `;
    }).join('');
}

function getStatusBadge(item, isUnit) {
    const now = Math.floor(Date.now() / 1000);
    const idleThreshold = 12 * 3600;
    const isIdle = (now - item.last_active) > idleThreshold;
    
    if (isUnit && !item.registered) {
        return '<span style="display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #666;" title="Deregistered"></span>';
    }
    
    if (isIdle) {
        return '<span style="display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #888;" title="Idle"></span>';
    }
    
    if (item.ever_encrypted) {
        return '<span style="display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #a83232;" title="Active (Encrypted)"></span>';
    }
    
    return '<span style="display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #32a852;" title="Active"></span>';
}

function make_unit_key(wacn, sysid, unit_id) {
    return `${wacn}:${sysid}:${unit_id}`;
}

function make_tg_key(wacn, sysid, tg_id) {
    return `${wacn}:${sysid}:${tg_id}`;
}

function jumpToTalkgroup(tgId, wacn, sysid) {
    affiliationState.currentView = 'talkgroups';
    affiliationState.searchTerm = '';
    const searchInput = document.getElementById('affiliationSearch');
    const clearBtn = document.getElementById('clearAffiliationSearch');
    if (searchInput) searchInput.value = '';
    if (clearBtn) clearBtn.style.display = 'none';
    showAffiliationView('talkgroups');
    
    // Scroll to the specific row after a short delay for rendering
    setTimeout(() => {
        const rowId = `aff-row-${wacn}-${sysid}-${tgId}`;
        const row = document.getElementById(rowId);
        if (row) {
            row.scrollIntoView({ behavior: 'smooth', block: 'center' });
            // Pulse animation: 3 quick flashes
            row.style.transition = 'background 0.3s ease';
            let pulseCount = 0;
            const pulseInterval = setInterval(() => {
                row.style.background = pulseCount % 2 === 0 ? 'rgba(59, 130, 246, 0.3)' : '';
                pulseCount++;
                if (pulseCount >= 6) {
                    clearInterval(pulseInterval);
                    row.style.background = '';
                    row.style.transition = '';
                }
            }, 300);
        }
    }, 100);
}

function jumpToUnit(unitId, wacn, sysid) {
    affiliationState.currentView = 'units';
    affiliationState.searchTerm = '';
    const searchInput = document.getElementById('affiliationSearch');
    const clearBtn = document.getElementById('clearAffiliationSearch');
    if (searchInput) searchInput.value = '';
    if (clearBtn) clearBtn.style.display = 'none';
    showAffiliationView('units');
    
    // Scroll to the specific row after a short delay for rendering
    setTimeout(() => {
        const rowId = `aff-row-${wacn}-${sysid}-${unitId}`;
        const row = document.getElementById(rowId);
        if (row) {
            row.scrollIntoView({ behavior: 'smooth', block: 'center' });
            // Pulse animation: 3 quick flashes
            row.style.transition = 'background 0.3s ease';
            let pulseCount = 0;
            const pulseInterval = setInterval(() => {
                row.style.background = pulseCount % 2 === 0 ? 'rgba(59, 130, 246, 0.3)' : '';
                pulseCount++;
                if (pulseCount >= 6) {
                    clearInterval(pulseInterval);
                    row.style.background = '';
                    row.style.transition = '';
                }
            }, 300);
        }
    }, 100);
}

function clearAffiliationSearch() {
    affiliationState.searchTerm = '';
    const searchInput = document.getElementById('affiliationSearch');
    const clearBtn = document.getElementById('clearAffiliationSearch');
    if (searchInput) searchInput.value = '';
    if (clearBtn) clearBtn.style.display = 'none';
    // Force full re-render and reset
    renderAffiliationTable();
}

function exportAffiliationsJSON() {
    const data = {
        exported_at: Math.floor(Date.now() / 1000),
        units: affiliationState.units,
        talkgroups: affiliationState.talkgroups
    };
    
    const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `affiliations_${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.json`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
}

// Initialize search handler
document.addEventListener('DOMContentLoaded', () => {
    const searchInput = document.getElementById('affiliationSearch');
    const clearBtn = document.getElementById('clearAffiliationSearch');
    
    if (searchInput) {
        searchInput.addEventListener('input', (e) => {
            affiliationState.searchTerm = e.target.value;
            if (clearBtn) {
                clearBtn.style.display = e.target.value ? 'block' : 'none';
            }
            renderAffiliationTable();
        });
    }
    
    // Smart periodic updates: refresh data every 30s but preserve user's sort/scroll position
    setInterval(() => {
        if (affiliationState.currentView) {
            // Save current scroll position
            const container = document.querySelector('.tab-content');
            const scrollPos = container ? container.scrollTop : 0;
            
            // Refresh data without changing sort
            loadAffiliations().then(() => {
                // Restore scroll position
                if (container) container.scrollTop = scrollPos;
            });
        }
    }, 30000); // Update every 30 seconds
});
