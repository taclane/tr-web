// tr-web: time-series chart (uPlot) shared by the Status and Systems tabs
//
//   const chart = new TimeChart(element, options);
//   chart.setData({ series, xMin, xMax });   // times in seconds
//
// series: [{ key, label, color, dash, points: [[t, value], ...] }]
// options:
//   height      pixels (default 220)
//   yStep       gridline interval (default 10); widened automatically if the range gets large
//   yMinMax     top of the y axis before auto-ranging above it (default 40)
//   stepped     draw values as steps held until the next sample (active calls)
//   decimals    readout precision (default 1)
//   yLabel      axis title
//   legendEl    element to render a clickable legend into (values shown next to each entry)
//   hidden      Set of series keys hidden by the user (toggled by legend clicks)
//   total       { label, color } adds a dashed series summing all series
//   onCursor    callback({ time, values: [{label, value}] } | null) for custom readouts

const TimeChart = (() => {
    const TOTAL_KEY = '__total__';

    function cssVar(name, fallback) {
        const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
        return v || fallback;
    }

    function fmtClock(t) {
        return new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    }

    function escapeText(s) {
        return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    }

    // Gridline step for a range: the configured step, doubled until there are at most 8 lines
    function stepFor(max, base) {
        let step = base;
        while (max / step > 8) step *= 2;
        return step;
    }

    // Put all series on one sorted time axis. Stepped series carry their last value forward;
    // others are null between samples.
    function align(series, stepped) {
        const times = new Set();
        series.forEach(s => s.points.forEach(p => times.add(p[0])));
        const xs = Array.from(times).sort((a, b) => a - b);
        const columns = series.map(s => {
            const byTime = new Map(s.points.map(p => [p[0], p[1]]));
            let last = null;
            return xs.map(t => {
                if (byTime.has(t)) {
                    last = byTime.get(t);
                    return last;
                }
                return stepped ? last : null;
            });
        });
        return [xs, columns];
    }

    class TimeChart {
        constructor(el, options = {}) {
            this.el = el;
            this.opts = Object.assign({ height: 220, yStep: 10, yMinMax: 40, stepped: false, decimals: 1, yLabel: '' }, options);
            this.hidden = this.opts.hidden || new Set();
            this.plot = null;
            this.seriesKeys = '';
            this.series = [];
            this.xRange = [0, 1];
            this.hoverIdx = null;
            this.resizeObserver = new ResizeObserver(() => {
                if (this.plot && this.el.clientWidth) this.plot.setSize({ width: this.el.clientWidth, height: this.opts.height });
            });
            this.resizeObserver.observe(el);
        }

        destroy() {
            this.resizeObserver.disconnect();
            if (this.plot) this.plot.destroy();
            this.plot = null;
        }

        setData({ series, xMin, xMax }) {
            let all = series.slice();
            if (this.opts.total && all.length > 0) {
                all.push({ key: TOTAL_KEY, label: this.opts.total.label, color: this.opts.total.color, dash: [4, 4], total: true, points: [] });
            }
            this.series = all;
            this.xRange = [xMin, xMax];

            const plain = all.filter(s => !s.total);
            const [xs, columns] = align(plain, this.opts.stepped);
            if (this.opts.total && all.length > plain.length) {
                columns.push(xs.map((_, i) => columns.reduce((sum, col) => sum + (col[i] || 0), 0)));
            }
            const data = [xs, ...columns];

            const keys = all.map(s => s.key).join('\u0001');
            if (!this.plot || keys !== this.seriesKeys) {
                this.seriesKeys = keys;
                this.create(data);
            } else {
                this.plot.setData(data); // rescales: y follows the data, x uses this.xRange
            }
            this.renderReadout();
        }

        create(data) {
            if (this.plot) this.plot.destroy();
            const o = this.opts;
            const axisColor = cssVar('--text-secondary', '#888');
            const gridColor = cssVar('--border', '#333');
            const paths = o.stepped ? uPlot.paths.stepped({ align: 1 }) : undefined;

            const opts = {
                width: this.el.clientWidth || 400,
                height: o.height,
                legend: { show: false },
                cursor: { y: false, points: { size: 6 } },
                scales: {
                    x: { time: true, range: () => this.xRange },
                    y: { range: (u, min, max) => {
                        const top = Math.max(o.yMinMax, max || 0);
                        const step = stepFor(top, o.yStep);
                        return [0, Math.ceil(top / step) * step];
                    } },
                },
                axes: [
                    {
                        stroke: axisColor, grid: { stroke: gridColor, width: 1 }, ticks: { stroke: gridColor }, font: '11px sans-serif',
                        space: 70,
                        // Plain hour:minute labels (uPlot's default adds seconds and date rows)
                        values: (u, splits) => splits.map(t => new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })),
                    },
                    {
                        stroke: axisColor, grid: { stroke: gridColor, width: 1 }, ticks: { show: false }, font: '11px sans-serif',
                        size: 44, label: o.yLabel || undefined, labelFont: '11px sans-serif', labelSize: o.yLabel ? 18 : 0,
                        splits: (u, axisIdx, min, max) => {
                            const step = stepFor(max, o.yStep);
                            const out = [];
                            for (let v = 0; v <= max + 1e-9; v += step) out.push(v);
                            return out;
                        },
                    },
                ],
                series: [{}].concat(this.series.map(s => ({
                    label: s.label,
                    stroke: s.color,
                    width: s.total ? 1.5 : 2,
                    dash: s.dash,
                    paths,
                    spanGaps: !o.stepped,
                    points: { show: false },
                    show: !this.hidden.has(s.key),
                }))),
                hooks: {
                    setCursor: [u => {
                        this.hoverIdx = u.cursor.idx === null || u.cursor.idx === undefined ? null : u.cursor.idx;
                        this.renderReadout();
                    }],
                },
            };
            this.el.innerHTML = '';
            this.plot = new uPlot(opts, data, this.el);
        }

        // Value of series i at idx, or the nearest earlier sample
        valueAt(i, idx) {
            const col = this.plot.data[i + 1];
            for (let j = idx; j >= 0 && idx - j < 50; j--) {
                if (col[j] !== null && col[j] !== undefined) return col[j];
            }
            return null;
        }

        renderReadout() {
            if (!this.plot) return;
            const xs = this.plot.data[0];
            const idx = this.hoverIdx !== null ? this.hoverIdx : xs.length - 1;
            const time = idx >= 0 ? xs[idx] : null;
            const values = this.series.map((s, i) => ({
                key: s.key,
                label: s.label,
                color: s.color,
                dash: !!s.dash,
                hidden: this.hidden.has(s.key),
                value: idx >= 0 ? this.valueAt(i, idx) : null,
            }));

            if (this.opts.onCursor) {
                this.opts.onCursor(time === null ? null : { time, hovering: this.hoverIdx !== null, values });
            }
            if (this.opts.legendEl) {
                this.renderLegend(values, time);
            }
        }

        renderLegend(values, time) {
            const el = this.opts.legendEl;
            const fmt = v => (v === null || v === undefined) ? '—' : Number(v).toFixed(this.opts.decimals);
            const when = time === null ? '' : (this.hoverIdx !== null ? fmtClock(time) : 'now');
            el.innerHTML =
                `<span class="legend-time">${escapeText(when)}</span>` +
                values.map(v => `
                    <div class="legend-item${v.hidden ? ' legend-hidden' : ''}" data-key="${escapeText(v.key)}" style="cursor:pointer; opacity:${v.hidden ? 0.4 : 1}">
                        <div class="legend-color" style="background:${escapeText(v.color)}${v.dash ? '; border: 1px dashed ' + escapeText(v.color) : ''}"></div>
                        ${escapeText(v.label)} <span class="legend-value">${v.hidden ? '' : escapeText(fmt(v.value))}</span>
                    </div>`).join('');
            el.querySelectorAll('.legend-item').forEach(item => {
                item.onclick = () => this.toggle(item.dataset.key);
            });
        }

        toggle(key) {
            const i = this.series.findIndex(s => s.key === key);
            if (i < 0) return;
            if (this.hidden.has(key)) this.hidden.delete(key);
            else this.hidden.add(key);
            this.plot.setSeries(i + 1, { show: !this.hidden.has(key) });
            this.renderReadout();
        }
    }

    TimeChart.TOTAL_KEY = TOTAL_KEY;
    return TimeChart;
})();
