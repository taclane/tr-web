// tr-web: Systems tab (Preact + htm, uPlot charts; see web/vendor/README.md)
// Rendered into #systemsRoot by renderSystemsTab(), which app.js calls on data updates.
// html`` output is escaped by Preact: never use innerHTML here.

(() => {
    const { h, render } = preact;
    const { useState, useEffect, useRef } = preactHooks;
    const html = htm.bind(h);

    const STATS_REFRESH_MS = 15000;
    const REFERENCE_ROW_LIMIT = 1000;

    function systemsTabActive() {
        const panel = document.getElementById('panel-systems');
        return !!panel && panel.classList.contains('active') && document.visibilityState === 'visible';
    }

    function cssVar(name, fallback) {
        const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
        return v || fallback;
    }

    // Five decimals: channel rasters are 6.25 kHz, which four would hide
    function fmtMHz(hz) {
        return hz ? (hz / 1e6).toFixed(5) + ' MHz' : '—';
    }

    // Bit error rate in %: corrected bits / voice bits. null without digital voice.
    function ber(errors, voiceBits) {
        return voiceBits > 0 ? 100 * errors / voiceBits : null;
    }

    function fmtBER(value) {
        if (value === null || value === undefined) return '—';
        if (value === 0) return '0%';
        if (value < 0.001) return '<0.001%';
        return value.toFixed(value < 0.1 ? 3 : value < 1 ? 2 : 1) + '%';
    }

    // Rough P25 guide: under 1% sounds clean, 1-2% is audible, above 2% degraded
    function berClass(value) {
        return value === null || value === undefined ? '' : value >= 2 ? 'ber-poor' : value >= 1 ? 'ber-fair' : '';
    }

    const BER_TITLE = 'Bit error rate: share of received voice bits the decoder had to correct';

    function BER({ value }) {
        return html`<span class=${berClass(value)}>${fmtBER(value)}</span>`;
    }

    function fmtDuration(seconds) {
        if (!seconds) return '0m';
        const d = Math.floor(seconds / 86400), hr = Math.floor((seconds % 86400) / 3600), m = Math.floor((seconds % 3600) / 60);
        if (d) return `${d}d ${hr}h`;
        if (hr) return `${hr}h ${m}m`;
        return `${m}m`;
    }

    function fmtAgo(epochSeconds) {
        if (!epochSeconds) return '—';
        return fmtDuration(Math.max(0, Math.floor(Date.now() / 1000) - epochSeconds)) + ' ago';
    }

    // ------------------------------------------------------------------ charts

    function fmtClock(epochSeconds) {
        return new Date(epochSeconds * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    }

    const PERIODS = [['5m', '5 min', 300], ['15m', '15 min', 900], ['60m', '60 min', 3600]];

    // [{time: ms, <key>: value}] -> [[seconds, value]] from `since` (seconds)
    function toPoints(history, key, since) {
        return (history || []).filter(p => p.time / 1000 >= since).map(p => [p.time / 1000, p[key]]);
    }

    // Chart card matching the Status tab (title, period buttons, chart, legend)
    function ChartCard({ title, label, history, valueKey, color, yStep, yMinMax, stepped = false, decimals = 1, yLabel }) {
        const [period, setPeriod] = useState('60m');
        const ref = useRef(null);
        const legendRef = useRef(null);
        const chart = useRef(null);

        useEffect(() => {
            chart.current = new TimeChart(ref.current, {
                height: 220, yStep, yMinMax, stepped, decimals, yLabel, legendEl: legendRef.current,
            });
            return () => {
                chart.current.destroy();
                chart.current = null;
            };
        }, []);

        useEffect(() => {
            const now = Date.now() / 1000;
            const span = PERIODS.find(p => p[0] === period)[2];
            chart.current.setData({
                series: [{ key: label, label, color, points: toPoints(history, valueKey, now - span) }],
                xMin: now - span,
                xMax: now,
            });
        });

        return html`
            <div class="card">
                <div class="card-header"><span class="card-title">${title}</span></div>
                <div class="chart-tabs">
                    ${PERIODS.map(([key, text]) => html`
                        <button type="button" class=${'chart-tab' + (key === period ? ' active' : '')} onClick=${() => setPeriod(key)}>${text}</button>`)}
                </div>
                <div class="chart-canvas-container chart-compact"><div class="chart-canvas" ref=${ref}></div></div>
                <div class="chart-legend" ref=${legendRef}></div>
            </div>`;
    }

    // Hourly chart for the details panel; the title reads out the hovered hour
    function HistoryChart({ title, points, xMin, color, yStep, yMinMax, stepped = false, decimals = 1, unit = '' }) {
        const ref = useRef(null);
        const chart = useRef(null);
        const [readout, setReadout] = useState(null);

        useEffect(() => {
            chart.current = new TimeChart(ref.current, { height: 140, yStep, yMinMax, stepped, decimals, onCursor: r => setReadout(r) });
            return () => {
                chart.current.destroy();
                chart.current = null;
            };
        }, []);

        useEffect(() => {
            chart.current.setData({
                series: [{ key: 'value', label: title, color, points }],
                xMin, xMax: Date.now() / 1000,
            });
        });

        const v = readout && readout.values[0] ? readout.values[0].value : null;
        const when = readout ? new Date(readout.time * 1000).toLocaleString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' }) : '';
        const text = v === null || v === undefined ? 'no data' : `${Number(v).toFixed(decimals)}${unit ? ' ' + unit : ''} · ${when}`;
        return html`
            <div class="sys-history">
                <div class="sys-spark-title">
                    <span>${title}</span>
                    <span class=${'sys-spark-readout' + (readout && readout.hovering ? ' hovering' : '')}>${text}</span>
                </div>
                <div ref=${ref}></div>
            </div>`;
    }

    // ------------------------------------------------------------ sortable table

    function SortableTable({ columns, rows, initialSort, rowClass, emptyText, rowKey, selectedKey, onRowClick }) {
        const [sort, setSort] = useState(initialSort);
        const col = columns.find(c => c.key === sort.key) || columns[0];
        const sorted = [...rows].sort((a, b) => {
            const av = col.sortValue ? col.sortValue(a) : a[col.key];
            const bv = col.sortValue ? col.sortValue(b) : b[col.key];
            if (av === bv) return 0;
            if (av === null || av === undefined) return 1; // blanks last either way
            if (bv === null || bv === undefined) return -1;
            return (av < bv ? -1 : 1) * (sort.dir === 'asc' ? 1 : -1);
        });
        const clickHeader = (key) => setSort(sort.key === key
            ? { key, dir: sort.dir === 'asc' ? 'desc' : 'asc' }
            : { key, dir: 'desc' });

        return html`
            <table class="sys-table">
                <thead><tr>
                    ${columns.map(c => html`
                        <th class=${c.numeric ? 'num' : ''} title=${c.title || ''}
                            onClick=${() => clickHeader(c.key)} style="cursor: pointer;">
                            ${c.label}${sort.key === c.key ? (sort.dir === 'asc' ? ' ▲' : ' ▼') : ''}
                        </th>`)}
                </tr></thead>
                <tbody>
                    ${sorted.length === 0
                        ? html`<tr><td colspan=${columns.length} class="sys-empty">${emptyText || 'No data yet'}</td></tr>`
                        : sorted.map(r => {
                            const key = rowKey ? rowKey(r) : null;
                            const classes = [rowClass ? rowClass(r) : '', key !== null && key === selectedKey ? 'sys-selected' : '', onRowClick ? 'sys-clickable' : '']
                                .filter(Boolean).join(' ');
                            return html`
                            <tr class=${classes} onClick=${onRowClick ? () => onRowClick(r) : undefined}>
                                ${columns.map(c => html`<td class=${c.numeric ? 'num' : ''}>${c.render ? c.render(r) : r[c.key]}</td>`)}
                            </tr>`;
                        })}
                </tbody>
            </table>`;
    }

    // --------------------------------------------------------------- panels

    // "restart" comes from memory, the others from the database's hourly totals
    const STATS_WINDOWS = [
        ['restart', 'Since restart'],
        ['24h', '24 hours'],
        ['7d', '7 days'],
        ['30d', '30 days'],
        ['all', 'All time'],
    ];

    function useSystemStats(sysNum, statsWindow) {
        const [stats, setStats] = useState(null);
        const [error, setError] = useState(null);
        useEffect(() => {
            // The previous window's numbers stay up until the new ones arrive (no layout jump)
            let cancelled = false;
            const load = async () => {
                if (!systemsTabActive()) return;
                try {
                    const r = await authenticatedFetch(`${BASE_PATH}api/system/stats?sys_num=${sysNum}&window=${statsWindow}`);
                    if (!r.ok) throw new Error(`HTTP ${r.status}`);
                    const data = await r.json();
                    if (!cancelled) { setStats(data); setError(null); }
                } catch (e) {
                    if (!cancelled) setError(e.message);
                }
            };
            load();
            const timer = setInterval(load, STATS_REFRESH_MS);
            // Opening the tab loads immediately instead of waiting for the next tick
            window.addEventListener('systems-tab-shown', load);
            return () => { cancelled = true; clearInterval(timer); window.removeEventListener('systems-tab-shown', load); };
        }, [sysNum, statsWindow]);
        return [stats, error];
    }

    function WindowSelector({ value, onChange }) {
        return html`
            <div class="sys-window">
                <span class="sys-note">Statistics:</span>
                ${STATS_WINDOWS.map(([key, label]) => html`
                    <button type="button" class=${'data-link' + (value === key ? ' active' : '')} onClick=${() => onChange(key)}>${label}</button>`)}
            </div>`;
    }

    function describeWindow(stats) {
        if (!stats) return '—';
        const since = new Date(stats.since * 1000).toLocaleString();
        return stats.window === 'restart' ? `since tr-web started (${since})` : `from ${since} to now`;
    }

    // Talkgroup / unit tag / OTA alias lists, loaded on demand
    const REFERENCE_KINDS = {
        talkgroups: {
            label: 'Talkgroups',
            rows: data => Array.isArray(data) ? data : [],
            columns: [
                { key: 'number', label: 'Number', numeric: true },
                { key: 'alpha_tag', label: 'Alpha tag' },
                { key: 'description', label: 'Description' },
                { key: 'tag', label: 'Tag' },
                { key: 'group', label: 'Group' },
                { key: 'priority', label: 'Priority', numeric: true },
            ],
        },
        unit_tags: {
            label: 'Unit tags',
            rows: data => (data && data.tags) || [],
            meta: data => data && `File: ${data.file || 'none'} · Mode: ${data.mode || 'default'}`,
            columns: [
                { key: 'pattern', label: 'Pattern (regex)' },
                { key: 'tag', label: 'Tag' },
            ],
        },
        unit_tags_ota: {
            label: 'OTA aliases',
            rows: data => (data && data.aliases) || [],
            meta: data => data && `File: ${data.file || 'none'}`,
            columns: [
                { key: 'unit', label: 'Unit', numeric: true },
                { key: 'alias', label: 'Alias' },
            ],
        },
    };

    // Service badges on the system buttons (same service split as tr-web's database keys)
    const SERVICE_BADGES = {
        p25: 'P25', conventionalP25: 'P25C', conventional: 'FM', dmr: 'DMR',
        conventionalDMR: 'DMRC', smartnet: 'SMARTNET', conventionalSIGMF: 'SIGMF',
    };
    const serviceBadge = type => SERVICE_BADGES[type] || String(type || '').toUpperCase();
    const isConventional = sys => String(sys.type || '').startsWith('conventional');

    // Per-frequency rows, derived from the statistics response
    function frequencyRows(stats) {
        return (stats ? stats.frequencies : []).map(f => {
            return {
                freq: f.freq,
                calls: f.all.calls,
                transmissions: f.all.transmissions,
                minutes: f.all.seconds / 60,
                ber: ber(f.all.errors, f.all.voice_bits),
                phase2: f.all.calls ? f.phase2_calls / f.all.calls : null,
                freqError: f.avg_freq_error,
                lastSeen: f.last_seen,
            };
        });
    }

    function talkgroupRows(stats, list = 'top_talkgroups') {
        return (stats ? stats[list] || [] : []).map(t => ({
            ...t,
            minutes: t.seconds / 60,
            encryptedShare: t.calls ? t.encrypted / t.calls : 0,
            ber: ber(t.errors, t.voice_bits),
        }));
    }

    function unitRows(stats) {
        return (stats ? stats.error_units || [] : []).map(u => ({
            ...u,
            minutes: u.seconds / 60,
            ber: ber(u.errors, u.voice_bits),
        }));
    }

    // Short window name for tile labels
    const windowLabel = key => ({ restart: 'since restart', all: 'all time' }[key] || key);

    // ---------------------------------------------------------------- stat tiles

    function StatTiles({ sys, stats, statsWindow }) {
        const name = sys.unique_sys_name;
        const rate = state.rates[name];
        const decodeRate = rate ? rate.decoderate : null;
        const activeCalls = state.calls.filter(c => c.sys_num === sys.sys_num).length;
        const cc = (rate && rate.control_channel) || sys.control_channel;
        const t = stats && stats.totals;
        // trunk-recorder's default controlWarnRate is 10 messages/s
        const rateClass = decodeRate === null ? '' : decodeRate < 10 ? 'sys-bad' : 'sys-good';
        const tile = (value, label, cls = '') => html`
            <div class="stat-box"><div class=${'stat-value ' + cls}>${value}</div><div class="stat-label">${label}</div></div>`;

        return html`
            <div class="stats-row sys-tiles">
                ${!isConventional(sys) && tile(decodeRate === null ? '—' : decodeRate.toFixed(1), 'Decode rate', rateClass)}
                ${tile(activeCalls, 'Active calls')}
                ${!isConventional(sys) && tile(fmtMHz(cc), 'Control channel')}
                ${tile(t ? t.calls : '—', 'Calls · ' + windowLabel(statsWindow))}
                ${tile(t ? fmtBER(ber(t.errors, t.voice_bits)) : '—', 'Bit error rate · ' + windowLabel(statsWindow),
                       t ? berClass(ber(t.errors, t.voice_bits)) : '')}
                ${tile(t && t.calls ? Math.round(100 * t.encrypted / t.calls) + '%' : '—', 'Encrypted · ' + windowLabel(statsWindow))}
                ${tile(t ? t.emergency : '—', 'Emergency · ' + windowLabel(statsWindow))}
            </div>`;
    }

    // ---------------------------------------------------------------- main views

    function ViewHeader({ title, children }) {
        return html`<div class="card-header"><span class="card-title">${title}</span><div class="card-header-right">${children}</div></div>`;
    }

    function ChannelsView({ stats, statsWindow, setStatsWindow, selection, select }) {
        const columns = [
            { key: 'freq', label: 'Frequency', render: r => fmtMHz(r.freq) },
            { key: 'calls', label: 'Calls', numeric: true },
            { key: 'ber', label: 'Bit errors', numeric: true, render: r => html`<${BER} value=${r.ber} />`, title: BER_TITLE },
            { key: 'phase2', label: 'Phase 2', numeric: true, render: r => r.phase2 === null ? '—' : Math.round(r.phase2 * 100) + '%',
              title: 'Share of calls on this channel that were Phase 2 TDMA' },
            { key: 'lastSeen', label: 'Last call', numeric: true, render: r => fmtAgo(r.lastSeen) },
        ];
        return html`
            <div>
                <${ViewHeader} title="Channel quality"><${WindowSelector} value=${statsWindow} onChange=${setStatsWindow} /><//>
                <${SortableTable} columns=${columns} rows=${frequencyRows(stats)} initialSort=${{ key: 'calls', dir: 'desc' }}
                    rowKey=${r => 'freq:' + r.freq} selectedKey=${selection} onRowClick=${r => select('freq:' + r.freq)}
                    emptyText="No completed calls recorded in this period" />
                <p class="sys-note">
                    All recorded calls on each channel, ${describeWindow(stats)}. Errors that stay with a channel, whichever talkgroups
                    and radios use it, point at this site's setup or at interference. Errors that follow particular talkgroups or radios
                    (see Top errors) came in with the signal: garbage in, garbage out. Select a row for more detail.
                </p>
            </div>`;
    }

    function TalkgroupsView({ stats, statsWindow, setStatsWindow, selection, select }) {
        const columns = [
            { key: 'talkgroup', label: 'Talkgroup', numeric: true },
            { key: 'alpha_tag', label: 'Alias', render: r => r.alpha_tag || '—' },
            { key: 'calls', label: 'Calls', numeric: true },
            { key: 'minutes', label: 'Airtime', numeric: true, render: r => r.minutes.toFixed(1), title: 'Minutes of recorded audio' },
            { key: 'ber', label: 'Bit errors', numeric: true, render: r => html`<${BER} value=${r.ber} />`, title: BER_TITLE },
        ];
        return html`
            <div>
                <${ViewHeader} title="Busiest talkgroups"><${WindowSelector} value=${statsWindow} onChange=${setStatsWindow} /><//>
                <${SortableTable} columns=${columns} rows=${talkgroupRows(stats)} initialSort=${{ key: 'minutes', dir: 'desc' }}
                    rowKey=${r => 'tg:' + r.talkgroup} selectedKey=${selection} onRowClick=${r => select('tg:' + r.talkgroup)}
                    emptyText="No completed calls recorded in this period" />
                <p class="sys-note">Top 25 by airtime, ${describeWindow(stats)}. Select a row for more detail.</p>
            </div>`;
    }

    // Talkgroups and radios with the most errors
    function ErrorsView({ stats, statsWindow, setStatsWindow, selection, select, errorsBy, setErrorsBy }) {
        const common = [
            { key: 'minutes', label: 'Airtime', numeric: true, render: r => r.minutes.toFixed(1), title: 'Minutes of recorded audio' },
            { key: 'errors', label: 'Errors', numeric: true, title: 'Voice bits the decoder corrected' },
            { key: 'ber', label: 'Bit errors', numeric: true, render: r => html`<${BER} value=${r.ber} />`, title: BER_TITLE },
        ];
        const byUnit = errorsBy === 'units';
        const columns = byUnit
            ? [{ key: 'unit', label: 'Radio', numeric: true },
               { key: 'alias', label: 'Alias', render: r => r.alias || '—' },
               { key: 'transmissions', label: 'Transmissions', numeric: true }, ...common]
            : [{ key: 'talkgroup', label: 'Talkgroup', numeric: true },
               { key: 'alpha_tag', label: 'Alias', render: r => r.alpha_tag || '—' },
               { key: 'calls', label: 'Calls', numeric: true }, ...common];
        const prefix = byUnit ? 'unit:' : 'tg:';
        const idOf = r => prefix + (byUnit ? r.unit : r.talkgroup);
        return html`
            <div>
                <${ViewHeader} title="Top errors"><${WindowSelector} value=${statsWindow} onChange=${setStatsWindow} /><//>
                <div class="sys-window sys-errors-by">
                    <span class="sys-note">By:</span>
                    <button type="button" class=${'data-link' + (byUnit ? '' : ' active')} onClick=${() => setErrorsBy('talkgroups')}>Talkgroup</button>
                    <button type="button" class=${'data-link' + (byUnit ? ' active' : '')} onClick=${() => setErrorsBy('units')}>Radio</button>
                </div>
                <${SortableTable} key=${errorsBy} columns=${columns} rows=${byUnit ? unitRows(stats) : talkgroupRows(stats, 'error_talkgroups')}
                    initialSort=${{ key: 'errors', dir: 'desc' }}
                    rowKey=${idOf} selectedKey=${selection} onRowClick=${r => select(idOf(r))}
                    emptyText="No decoder errors recorded in this period" />
                <p class="sys-note">
                    The 25 ${byUnit ? 'radios' : 'talkgroups'} with the most corrected voice bits, ${describeWindow(stats)}.
                    Errors that follow a talkgroup or radio arrived that way (portables on a fireground, units in tunnels or moving
                    quickly between sites): garbage in, garbage out. Errors that stay with a channel instead (see Channels) point at
                    this site's setup or at interference. Sort by bit errors to compare rates; entries with little airtime vary a lot.
                </p>
            </div>`;
    }

    function SiteView({ sys }) {
        const channels = Array.isArray(sys.control_channels) ? sys.control_channels : [];
        const rows = [
            ['Type', sys.type],
            ['System ID', sys.sysid],
            ['WACN', sys.wacn],
            ['NAC', sys.nac],
            ['RFSS', sys.rfss],
            ['Site', sys.site_id],
            ['Talkgroups file', sys.talkgroups_file],
            ['Unit tags file', sys.unit_tags_file],
            ['OTA aliases file', sys.unit_tags_ota_file],
        ];
        return html`
            <div>
                <${ViewHeader} title="Site" />
                <table class="sys-kv">
                    ${rows.map(([k, v]) => html`<tr><th>${k}</th><td>${v === undefined || v === null || v === '' ? '—' : v}</td></tr>`)}
                </table>
                ${channels.length > 0 && html`
                    <h4>Control channels</h4>
                    <div class="control-channels">
                        ${channels.map(cc => html`<div class=${'cc-item' + (cc === sys.control_channel ? ' active' : '')}>${fmtMHz(cc)}</div>`)}
                    </div>`}
            </div>`;
    }

    // Talkgroup list, unit tags or OTA aliases for one system
    function ReferenceView({ sys, kind }) {
        // The list on screen stays until its replacement has loaded (no layout jump)
        const [shown, setShown] = useState(null); // {kind, data}
        const [loading, setLoading] = useState(false);
        const [error, setError] = useState(null);
        const [filter, setFilter] = useState('');

        useEffect(() => {
            let cancelled = false;
            setLoading(true); setError(null);
            (async () => {
                try {
                    const r = await authenticatedFetch(`${BASE_PATH}api/system/${kind}?sys_num=${sys.sys_num}`);
                    if (!r.ok) throw new Error(`HTTP ${r.status}`);
                    const data = await r.json();
                    if (!cancelled) { setShown({ kind, data }); setFilter(''); }
                } catch (e) {
                    if (!cancelled) setError(e.message);
                } finally {
                    if (!cancelled) setLoading(false);
                }
            })();
            return () => { cancelled = true; };
        }, [sys.sys_num, kind]);

        const spec = REFERENCE_KINDS[kind];
        const shownSpec = shown ? REFERENCE_KINDS[shown.kind] : null;
        let rows = shownSpec ? shownSpec.rows(shown.data) : [];
        const total = rows.length;
        if (filter && shownSpec) {
            const term = filter.toLowerCase();
            rows = rows.filter(r => shownSpec.columns.some(c => String(r[c.key] ?? '').toLowerCase().includes(term)));
        }
        const matched = rows.length;
        rows = rows.slice(0, REFERENCE_ROW_LIMIT);

        return html`
            <div>
                <${ViewHeader} title=${spec.label}>${loading && html`<span class="sys-note">Loading…</span>`}<//>
                ${error && html`<p class="error">Error loading data: ${error}</p>`}
                ${shownSpec && html`
                    <div class="sys-ref-bar">
                        <input type="search" placeholder="Filter" value=${filter} onInput=${e => setFilter(e.target.value)} />
                        <span class="sys-note">
                            ${shownSpec.meta ? shownSpec.meta(shown.data) + ' · ' : ''}${matched === total ? `${total} entries` : `${matched} of ${total} entries`}
                            ${matched > REFERENCE_ROW_LIMIT ? ` · showing the first ${REFERENCE_ROW_LIMIT}` : ''}
                        </span>
                    </div>
                    <div class="sys-ref-scroll">
                        <${SortableTable} key=${shown.kind} columns=${shownSpec.columns} rows=${rows}
                            initialSort=${{ key: shownSpec.columns[0].key, dir: 'asc' }} emptyText="No entries" />
                    </div>`}
            </div>`;
    }

    // ---------------------------------------------------------------- details

    function useHistory(sys, selection, statsWindow) {
        const [history, setHistory] = useState(null);
        useEffect(() => {
            setHistory(null);
            if (!selection) return;
            const [kind, id] = selection.split(':');
            let cancelled = false;
            const load = async () => {
                if (!systemsTabActive()) return;
                try {
                    const r = await authenticatedFetch(`${BASE_PATH}api/system/history?sys_num=${sys.sys_num}` +
                        `&kind=${{ tg: 'talkgroup', unit: 'unit' }[kind] || 'freq'}&id=${id}&window=${statsWindow}`);
                    const data = r.ok ? await r.json() : { unavailable: true };
                    if (!cancelled) setHistory(data);
                } catch (e) {
                    if (!cancelled) setHistory({ unavailable: true });
                }
            };
            load();
            const timer = setInterval(load, 60000);
            return () => { cancelled = true; clearInterval(timer); };
        }, [sys.sys_num, selection, statsWindow]);
        return history;
    }

    function KeyValues({ rows }) {
        return html`<table class="sys-kv">${rows.map(([k, v]) => html`<tr><th>${k}</th><td>${v}</td></tr>`)}</table>`;
    }

    function DetailsPanel({ sys, stats, statsWindow, selection }) {
        const history = useHistory(sys, selection, statsWindow);
        if (!selection) {
            return html`<div><${ViewHeader} title="Details" /><p class="sys-empty">Select a channel, talkgroup or radio to see details</p></div>`;
        }
        const [kind, id] = selection.split(':');
        const hours = history && history.hours ? history.hours : [];
        const xMin = stats ? stats.since : Date.now() / 1000 - 86400;
        const green = cssVar('--accent-green', '#66bb6a');
        const cyan = cssVar('--accent-cyan', '#4fc3f7');
        const callsPerHour = hours.map(h => [h.hour, h.calls]);
        const berChart = html`
            <${HistoryChart} title="Bit error rate by hour" points=${hours.map(h => [h.hour, ber(h.errors, h.voice_bits) || 0])}
                xMin=${xMin} color=${green} yStep=${0.5} yMinMax=${2} stepped=${true} decimals=${3} unit="%" />`;
        const historyOrNote = charts => history && history.unavailable
            ? html`<p class="sys-note">Hourly history needs tr-web's database.</p>` : charts;
        let body, charts;

        if (kind === 'freq') {
            const r = frequencyRows(stats).find(x => String(x.freq) === id);
            body = r ? html`<${KeyValues} rows=${[
                ['Calls', r.calls],
                ['Transmissions', r.transmissions],
                ['Audio', r.minutes.toFixed(1) + ' min'],
                ['Bit error rate', html`<${BER} value=${r.ber} />`],
                ['Phase 2', r.phase2 === null ? '—' : Math.round(r.phase2 * 100) + '%'],
                ['Avg tuning error', r.freqError + ' Hz'],
                ['Last call', fmtAgo(r.lastSeen)],
            ]} />` : html`<p class="sys-note">No calls on this frequency in this period.</p>`;
            charts = html`
                <${HistoryChart} title="Calls per hour" points=${callsPerHour} xMin=${xMin} color=${cyan} yStep=${5} yMinMax=${5} stepped=${true} decimals=${0} />
                ${berChart}`;
            return html`
                <div>
                    <${ViewHeader} title=${fmtMHz(Number(id))} />
                    ${body}
                    ${historyOrNote(charts)}
                </div>`;
        }

        if (kind === 'unit') {
            const u = unitRows(stats).find(x => String(x.unit) === id);
            body = u ? html`<${KeyValues} rows=${[
                ['Alias', u.alias || '—'],
                ['Transmissions', u.transmissions],
                ['Airtime', u.minutes.toFixed(1) + ' min'],
                ['Errors', u.errors],
                ['Bit error rate', html`<${BER} value=${u.ber} />`],
                ['Last heard', fmtAgo(u.last_seen)],
            ]} />` : html`<p class="sys-note">Not among the radios with the most errors in this period.</p>`;
            charts = html`
                <${HistoryChart} title="Transmissions per hour" points=${hours.map(h => [h.hour, h.transmissions])} xMin=${xMin} color=${cyan}
                    yStep=${5} yMinMax=${5} stepped=${true} decimals=${0} />
                ${berChart}`;
            return html`
                <div>
                    <${ViewHeader} title=${'Radio ' + id} />
                    ${body}
                    ${historyOrNote(charts)}
                </div>`;
        }

        const t = talkgroupRows(stats).find(x => String(x.talkgroup) === id) ||
                  talkgroupRows(stats, 'error_talkgroups').find(x => String(x.talkgroup) === id);
        body = t ? html`<${KeyValues} rows=${[
            ['Alias', t.alpha_tag || '—'],
            ['Calls', t.calls],
            ['Airtime', t.minutes.toFixed(1) + ' min'],
            ['Encrypted', Math.round(t.encryptedShare * 100) + '%'],
            ['Emergency', t.emergency],
            ['Bit error rate', html`<${BER} value=${t.ber} />`],
            ['Last call', fmtAgo(t.last_seen)],
        ]} />` : html`<p class="sys-note">Not among the busiest talkgroups in this period.</p>`;
        charts = html`
            <${HistoryChart} title="Calls per hour" points=${callsPerHour} xMin=${xMin} color=${cyan} yStep=${5} yMinMax=${5} stepped=${true} decimals=${0} />
            <${HistoryChart} title="Airtime per hour (min)" points=${hours.map(h => [h.hour, h.seconds / 60])}
                xMin=${xMin} color=${green} yStep=${5} yMinMax=${5} stepped=${true} decimals=${1} unit="min" />
            ${berChart}`;
        return html`
            <div>
                <${ViewHeader} title=${'Talkgroup ' + id} />
                ${body}
                ${historyOrNote(charts)}
            </div>`;
    }

    // ---------------------------------------------------------------- page

    const SIDE_TABS = [
        ['channels', 'Channels'],
        ['talkgroups', 'Talkgroups'],
        ['errors', 'Top errors'],
        ['site', 'Site'],
        ['ref:talkgroups', 'Talkgroup list', 'Reference'],
        ['ref:unit_tags', 'Unit tags'],
        ['ref:unit_tags_ota', 'OTA aliases'],
    ];

    function SystemPanel({ sys, statsWindow, setStatsWindow, sideTab, setSideTab, errorsBy, setErrorsBy }) {
        const [stats, statsError] = useSystemStats(sys.sys_num, statsWindow);
        const [selection, setSelection] = useState(null); // 'freq:<hz>', 'tg:<id>' or 'unit:<id>'
        const name = sys.unique_sys_name;
        const conventional = isConventional(sys);
        const tabs = SIDE_TABS.filter(([key]) => key !== 'site' || !conventional);
        const tab = tabs.some(t => t[0] === sideTab) ? sideTab : 'channels';

        let main;
        if (tab === 'channels') main = html`<${ChannelsView} stats=${stats} statsWindow=${statsWindow} setStatsWindow=${setStatsWindow} selection=${selection} select=${setSelection} />`;
        else if (tab === 'talkgroups') main = html`<${TalkgroupsView} stats=${stats} statsWindow=${statsWindow} setStatsWindow=${setStatsWindow} selection=${selection} select=${setSelection} />`;
        else if (tab === 'errors') main = html`<${ErrorsView} stats=${stats} statsWindow=${statsWindow} setStatsWindow=${setStatsWindow} selection=${selection} select=${setSelection} errorsBy=${errorsBy} setErrorsBy=${setErrorsBy} />`;
        else if (tab === 'site') main = html`<${SiteView} sys=${sys} />`;
        else main = html`<${ReferenceView} sys=${sys} kind=${tab.slice(4)} />`;

        return html`
            <div class="sys-panel">
                <${StatTiles} sys=${sys} stats=${stats} statsWindow=${statsWindow} />
                ${statsError && html`<p class="error">Could not load statistics: ${statsError}</p>`}
                <div class=${'status-grid' + (conventional ? ' sys-single-chart' : '')}>
                    ${!conventional && html`
                        <${ChartCard} title="Decode Rate" label=${name} history=${state.rateHistory[name]} valueKey="rate"
                            color=${cssVar('--accent-green', '#66bb6a')} yStep=${10} yMinMax=${40} yLabel="Decode rate (msg/s)" />`}
                    <${ChartCard} title="Active Calls" label=${name} history=${state.callRateHistory[name]} valueKey="count"
                        color=${cssVar('--accent-cyan', '#4fc3f7')} yStep=${5} yMinMax=${5} stepped=${true} decimals=${0} yLabel="Active calls" />
                </div>
                <div class="sys-lower">
                    <nav class="card sys-sidenav">
                        ${tabs.map(([key, label, group]) => html`
                            ${group && html`<div class="sys-sidenav-group">${group}</div>`}
                            <button type="button" class=${key === tab ? 'active' : ''} onClick=${() => setSideTab(key)}>${label}</button>`)}
                    </nav>
                    <div class="card sys-main">${main}</div>
                    <div class="card sys-details">
                        <${DetailsPanel} sys=${sys} stats=${stats} statsWindow=${statsWindow} selection=${selection} />
                    </div>
                </div>
            </div>`;
    }

    function SystemsApp() {
        const [selected, setSelected] = useState(0);
        const [statsWindow, setStatsWindow] = useState('restart');
        const [sideTab, setSideTab] = useState('channels');
        const [errorsBy, setErrorsBy] = useState('talkgroups');
        const systems = state.systems || [];
        if (systems.length === 0) {
            return html`<p class="sys-empty">No systems configured</p>`;
        }
        const index = Math.min(selected, systems.length - 1);
        const sys = systems[index];
        return html`
            <div class="tabs sys-picker">
                ${systems.map((s, i) => html`
                    <div class=${'tab' + (i === index ? ' active' : '')} onClick=${() => setSelected(i)}>
                        ${s.sys_name}<span class="sys-badge" title=${s.type}>${serviceBadge(s.type)}</span>
                    </div>`)}
            </div>
            <${SystemPanel} key=${sys.sys_num} sys=${sys} statsWindow=${statsWindow} setStatsWindow=${setStatsWindow}
                sideTab=${sideTab} setSideTab=${setSideTab} errorsBy=${errorsBy} setErrorsBy=${setErrorsBy} />`;
    }

    // Redraw at most every 500 ms, and only while the tab is showing
    let pending = false;
    window.renderSystemsTab = function () {
        const root = document.getElementById('systemsRoot');
        if (!root || pending) return;
        pending = true;
        setTimeout(() => {
            pending = false;
            if (systemsTabActive() || !root.hasChildNodes()) {
                render(html`<${SystemsApp} />`, root);
            }
        }, 500);
    };
})();
