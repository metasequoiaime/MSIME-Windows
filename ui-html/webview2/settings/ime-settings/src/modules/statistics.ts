import { onHostMessage } from '../utils/host-messages';
import { serializeHostMessage } from '../../../../shared/messages';
import type { ServerMessage, SettingsMessage } from '../../../../shared/messages';
import { applyToggleState, setupToggleButton } from './shared';
import { updateConfig } from './config-sync';
import {
  averageCharsPerDay,
  buildCalendar,
  charsPerMinute,
  currentStreak,
  dayKeyFromDate,
  fastestHour,
  findDay,
  formatDayKey,
  highestDay,
  longestStreak,
  sumDaily,
  totalChars,
  type CharacterBreakdown,
  type DailyStats,
  type HourlyStats,
} from '../utils/statistics-metrics';

type StatsRequest = Extract<SettingsMessage, { type: 'statsRequest' }>['data'];
type StatsResponse = Extract<ServerMessage, { type: 'statsResponse' }>['data'];
type StatsRecentRequest = Extract<SettingsMessage, { type: 'statsRecentRequest' }>['data'];
type StatsRecentResponse = Extract<ServerMessage, { type: 'statsRecentResponse' }>['data'];
type RecentWindow = StatsRecentResponse['m5'];

// 与 [statistics].retention 同值域；下拉是唯一入口，选中即写入配置，Server 回收到后才生效。
const RETENTION_VALUES = ['30d', '90d', '180d', '365d', 'forever'] as const;
type Retention = (typeof RETENTION_VALUES)[number];

function isRetention(value: unknown): value is Retention {
  return typeof value === 'string' && (RETENTION_VALUES as readonly string[]).includes(value);
}

const BREAKDOWN_LABELS: Array<[keyof CharacterBreakdown, string]> = [
  ['cjk', '中文'],
  ['latin', '英文'],
  ['digit', '数字'],
  ['punct', '标点'],
  ['other', '其他'],
];

let requestCounter = 0;
// 只认最后一个请求的回包：连续刷新时，旧回包会把新数据盖回去。
let latestRequestId = '';
// 实时窗口有自己的一套请求序列：与历史查询各自丢过期回包，互不干扰。
let recentRequestCounter = 0;
let latestRecentRequestId = '';
let recentTimer: number | null = null;

// 窗口内活跃时长低于此值就不给数：含义是「至少要有一段真实输入」——
// 没有这个门槛，「2 个字符间隔 0.2 秒」会算出 600 字/分钟。
// 注意它不等于 DLL 的停顿阈值（那个是 10 秒，决定一段输入算不算同一段）；
// 这里是防小样本的最低量，两者目的不同。
const RECENT_MIN_ACTIVE_MS = 5_000;
// 面板可见时按秒刷新；只写这三个数字，不重绘图表、不重新请求历史数据。
const RECENT_REFRESH_INTERVAL_MS = 1_000;
// Server 端生效的保留策略，用来发现「策略变了」：改策略会立即清理一次，面板必须重新取数，
// 否则下拉写着「保留最近 30 天」而图表还画着清理前的全量数据。
let appliedRetention: string | null = null;

function byId(id: string): HTMLElement | null {
  return document.getElementById(id);
}

function setText(id: string, text: string): void {
  const node = byId(id);
  if (node) node.textContent = text;
}

function formatChars(value: number): string {
  return String(Math.round(value)).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

function formatSpeed(perMinute: number): string {
  if (!(perMinute > 0)) return '—';
  return perMinute >= 10 ? String(Math.round(perMinute)) : perMinute.toFixed(1);
}

function formatDuration(activeMs: number): string {
  const minutes = activeMs / 60_000;
  if (minutes < 1) return '不到 1 分钟';
  if (minutes < 60) return `${Math.round(minutes)} 分钟`;
  const hours = Math.floor(minutes / 60);
  const rest = Math.round(minutes - hours * 60);
  return rest > 0 ? `${hours} 小时 ${rest} 分钟` : `${hours} 小时`;
}

function setEnabledState(enabled: boolean): void {
  applyToggleState('statisticsToggleBtn', enabled);
  byId('statisticsDisabledHint')?.classList.toggle('is-hidden', enabled);
}

function showEmpty(title: string, description: string): void {
  setText('statisticsEmptyTitle', title);
  setText('statisticsEmptyDescription', description);
  byId('statisticsEmpty')?.classList.remove('is-hidden');
  byId('statisticsData')?.classList.add('is-hidden');
}

function hideEmpty(): void {
  byId('statisticsEmpty')?.classList.add('is-hidden');
  byId('statisticsData')?.classList.remove('is-hidden');
}

function renderCards(daily: DailyStats[], today: number): void {
  const totals = sumDaily(daily);
  const todayRow = findDay(daily, today);
  const todayChars = todayRow ? totalChars(todayRow) : 0;

  setText('statisticsTodayChars', `${formatChars(todayChars)} 字`);
  setText('statisticsTodaySub', todayChars > 0 ? `活跃 ${formatDuration(todayRow?.activeMs ?? 0)}` : '今天还没有输入');
  setText('statisticsTotalChars', `${formatChars(totals.chars)} 字`);
  setText('statisticsTotalSub', `${totals.recordedDays} 天有输入记录`);
  setText('statisticsAverageChars', `${formatChars(averageCharsPerDay(daily))} 字`);
  setText('statisticsAverageSub', '按有输入记录的天数计算');
  setText('statisticsStreakDays', `${currentStreak(daily, today)} 天`);
  setText('statisticsStreakSub', `历史最长 ${longestStreak(daily)} 天`);
}

function renderCalendar(daily: DailyStats[], today: number): void {
  const container = byId('statisticsCalendarGrid');
  if (!container) return;
  const grid = buildCalendar(daily, today);
  // 月份标签与日期格共用一个网格：标签占第 1 行，日期占第 2–8 行，行高与列宽都由模板固定。
  container.style.gridTemplateColumns = `repeat(${grid.columns}, 12px)`;

  const nodes: HTMLElement[] = grid.monthLabels.map(({ column, label }) => {
    const node = document.createElement('div');
    node.className = 'statistics-calendar-month';
    node.style.gridRow = '1';
    node.style.gridColumn = String(column + 1);
    node.textContent = label;
    return node;
  });

  grid.cells.forEach((cell, index) => {
    const node = document.createElement('div');
    node.className = `statistics-heat level-${cell.level}${cell.inRange ? '' : ' is-outside'}`;
    node.style.gridRow = String((index % 7) + 2);
    node.style.gridColumn = String(Math.floor(index / 7) + 1);
    if (cell.inRange) {
      node.title = `${formatDayKey(cell.day)} · ${formatChars(cell.chars)} 字`;
    }
    nodes.push(node);
  });

  container.replaceChildren(...nodes);
}

function renderHourly(hourly: HourlyStats[], today: number): void {
  const container = byId('statisticsHourly');
  if (!container) return;

  const charsByHour = new Map<number, number>();
  let todayChars = 0;
  for (const row of hourly) {
    if (row.day !== today) continue;
    charsByHour.set(row.hour, (charsByHour.get(row.hour) ?? 0) + row.chars);
    todayChars += row.chars;
  }
  const maxChars = Math.max(1, ...charsByHour.values());

  const nodes: HTMLElement[] = [];
  for (let hour = 0; hour < 24; hour += 1) {
    const chars = charsByHour.get(hour) ?? 0;
    const column = document.createElement('div');
    column.className = 'statistics-hourly-column';
    const track = document.createElement('div');
    track.className = 'statistics-hourly-track';
    const bar = document.createElement('div');
    bar.className = chars > 0 ? 'statistics-hourly-bar' : 'statistics-hourly-bar is-empty';
    bar.style.height = `${Math.round((chars / maxChars) * 100)}%`;
    bar.title = `${String(hour).padStart(2, '0')}:00–${String(hour).padStart(2, '0')}:59 · ${formatChars(chars)} 字`;
    track.appendChild(bar);
    const label = document.createElement('div');
    label.className = 'statistics-hourly-label';
    label.textContent = hour % 3 === 0 ? String(hour) : '';
    column.append(track, label);
    nodes.push(column);
  }
  container.replaceChildren(...nodes);

  setText('statisticsHourlyTotal', todayChars > 0 ? `今日共 ${formatChars(todayChars)} 字` : '今天还没有输入');
}

function renderBreakdown(daily: DailyStats[]): void {
  const container = byId('statisticsBreakdown');
  if (!container) return;
  const totals = sumDaily(daily);

  container.replaceChildren(...BREAKDOWN_LABELS.map(([key, label]) => {
    const value = totals[key];
    const ratio = totals.chars > 0 ? value / totals.chars : 0;

    const row = document.createElement('div');
    row.className = 'statistics-breakdown-row';
    const name = document.createElement('span');
    name.className = 'statistics-breakdown-label';
    name.textContent = label;
    const track = document.createElement('div');
    track.className = 'statistics-breakdown-track';
    const fill = document.createElement('div');
    fill.className = 'statistics-breakdown-fill';
    fill.style.width = `${(ratio * 100).toFixed(1)}%`;
    track.appendChild(fill);
    const amount = document.createElement('span');
    amount.className = 'statistics-breakdown-value';
    amount.textContent = `${formatChars(value)} · ${(ratio * 100).toFixed(1)}%`;
    row.append(name, track, amount);
    return row;
  }));
}

function renderDetails(daily: DailyStats[], hourly: HourlyStats[], today: number): void {
  const container = byId('statisticsDetails');
  if (!container) return;
  const totals = sumDaily(daily);
  const todayRow = findDay(daily, today);
  const todayChars = todayRow ? totalChars(todayRow) : 0;
  const best = highestDay(daily);
  const fastest = fastestHour(hourly);

  const rows: Array<[string, string]> = [
    ['最高日', best ? `${formatChars(best.chars)} 字 · ${formatDayKey(best.day)}` : '—'],
    [
      '今日速度',
      todayChars > 0 ? `${formatSpeed(charsPerMinute(todayChars, todayRow?.activeMs ?? 0))} 字/分钟` : '—',
    ],
    [
      '平均速度',
      totals.chars > 0 ? `${formatSpeed(charsPerMinute(totals.chars, totals.activeMs))} 字/分钟` : '—',
    ],
    [
      '最快速度',
      fastest
        ? `${formatSpeed(fastest.perMinute)} 字/分钟 · ${formatDayKey(fastest.day)} ${String(fastest.hour).padStart(2, '0')}:00`
        : '—',
    ],
    ['累计活跃时长', totals.activeMs > 0 ? formatDuration(totals.activeMs) : '—'],
    ['有输入记录的天数', `${totals.recordedDays} 天`],
  ];

  container.replaceChildren(...rows.map(([label, value]) => {
    const row = document.createElement('div');
    row.className = 'statistics-detail-row';
    const name = document.createElement('span');
    name.className = 'statistics-detail-label';
    name.textContent = label;
    const amount = document.createElement('span');
    amount.className = 'statistics-detail-value';
    amount.textContent = value;
    row.append(name, amount);
    return row;
  }));
}

function renderPanel(data: StatsResponse): void {
  const daily: DailyStats[] = Array.isArray(data.daily) ? data.daily : [];
  const hourly: HourlyStats[] = Array.isArray(data.hourly) ? data.hourly : [];
  const today = dayKeyFromDate(new Date());
  // 空状态判据是 meta.firstDay（Server 在清理后重算），不是「全 0 数据」。
  const hasRecords = typeof data.meta?.firstDay === 'number' && daily.length > 0;

  if (typeof data.meta?.enabled === 'boolean') setEnabledState(data.meta.enabled);

  if (!hasRecords) {
    showEmpty(
      data.ok ? '还没有统计数据' : '统计数据读取失败',
      data.ok ? '开启统计后正常打字，这里会显示字数、输入日历与时段分布。' : (data.message ?? '请稍后重试。'),
    );
    return;
  }

  hideEmpty();
  renderCards(daily, today);
  renderCalendar(daily, today);
  renderHourly(hourly, today);
  renderBreakdown(daily);
  renderDetails(daily, hourly, today);
}

function requestStats(): void {
  const requestId = `stats-${++requestCounter}`;
  latestRequestId = requestId;
  const data: StatsRequest = { requestId, action: 'query' };
  window.chrome?.webview?.postMessage(serializeHostMessage({ type: 'statsRequest', data }));
}

function handleResponse(data: StatsResponse): void {
  if (data.requestId !== latestRequestId) return;
  renderPanel(data);
}

function formatRecentSpeed(window: RecentWindow | undefined): string {
  if (!window) return '—';
  const chars = Number(window.chars);
  const activeMs = Number(window.activeMs);
  if (!Number.isFinite(chars) || !Number.isFinite(activeMs)) return '—';
  if (activeMs < RECENT_MIN_ACTIVE_MS) return '—';
  return `${formatSpeed(charsPerMinute(chars, activeMs))} 字/分钟`;
}

function requestRecent(): void {
  const requestId = `stats-recent-${++recentRequestCounter}`;
  latestRecentRequestId = requestId;
  const data: StatsRecentRequest = { requestId };
  window.chrome?.webview?.postMessage(serializeHostMessage({ type: 'statsRecentRequest', data }));
}

function handleRecentResponse(data: StatsRecentResponse): void {
  // 过期回包直接丢弃；失败（ok=false）时保留上一次的数值，不清零、不弹错。
  if (data.requestId !== latestRecentRequestId) return;
  if (!data.ok) return;
  setText('statisticsRecentM5', formatRecentSpeed(data.m5));
  setText('statisticsRecentH1', formatRecentSpeed(data.h1));
  setText('statisticsRecentD1', formatRecentSpeed(data.d1));
}

function isPanelVisible(): boolean {
  const container = byId('statistics');
  return container !== null && container.style.display === 'block';
}

function stopRecentRefresh(): void {
  if (recentTimer !== null) {
    window.clearInterval(recentTimer);
    recentTimer = null;
  }
}

function startRecentRefresh(): void {
  // 装上之前先清旧的：切换面板会重复触发 msime:module-shown，装两次定时器会让请求翻倍。
  stopRecentRefresh();
  if (!isPanelVisible()) return;
  requestRecent();
  recentTimer = window.setInterval(() => {
    // 没有 module-hidden 事件，所以每个 tick 自查一次可见性；不可见就停表，不再空转。
    if (!isPanelVisible()) {
      stopRecentRefresh();
      return;
    }
    requestRecent();
  }, RECENT_REFRESH_INTERVAL_MS);
}

function setupRefreshHooks(): void {
  // 模块只装配一次，重新打开面板不会重跑 setup：不主动刷新的话数字会停在首次加载的时刻。
  window.addEventListener('msime:module-shown', (event: Event) => {
    if ((event as CustomEvent<{ module?: string }>).detail?.module === 'statistics') {
      requestStats();
      startRecentRefresh();
    }
  });
  window.addEventListener('focus', () => {
    if (isPanelVisible()) requestStats();
  });
}

/** config-sync 把 configSnapshot 的 `statistics.enabled` / `statistics.retention` 交到这里回填。 */
export function applyStatisticsConfig(enabled: unknown, retention?: unknown): void {
  if (typeof enabled === 'boolean') setEnabledState(enabled);
  // 非白名单值跳过：下拉永远显示 Server 真正生效的策略，而不是一个前端造出来的值。
  if (isRetention(retention)) {
    const select = byId('statisticsRetention') as HTMLSelectElement | null;
    if (select) select.value = retention;
    // 首次回填不重查（setup 已经取过一次），只在策略真的变了时重取：
    // Server 在写配置成功后立即按新策略清理，快照回到这里时数据已经变少，
    // 不重查的话图表会停留在清理前的样子。
    const changed = appliedRetention !== null && appliedRetention !== retention;
    appliedRetention = retention;
    if (changed) requestStats();
  }
}

export function setupStatistics(): void {
  setupToggleButton('statisticsToggleBtn', (active) => {
    setEnabledState(active);
    updateConfig('statistics.enabled', active);
  });
  // 下拉只负责设定保留策略（持久生效），不发送任何清理请求：改选后由 Server 落盘并立即清理一次。
  byId('statisticsRetention')?.addEventListener('change', () => {
    const select = byId('statisticsRetention') as HTMLSelectElement | null;
    if (select && isRetention(select.value)) updateConfig('statistics.retention', select.value);
  });
  onHostMessage('statsResponse', (message) => handleResponse(message.data));
  onHostMessage('statsRecentResponse', (message) => handleRecentResponse(message.data));
  setupRefreshHooks();
  requestStats();
  // setup 跑在面板变可见之前（sidebar 先 loadContent 再 showOnlyCurrentModule），所以这里通常
  // 不会启动轮询；真正启动它的是紧随其后的 msime:module-shown。若将来改成先显示后加载，这里
  // 也能自洽地工作。
  startRecentRefresh();
}
