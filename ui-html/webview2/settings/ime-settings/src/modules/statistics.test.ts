import { afterEach, beforeEach, expect, it, vi } from 'vitest';
// 源文件以字符串参与断言（Vite 的 ?raw），用于确认「手动清理按钮」不会被重新加回来。
import statisticsHtml from '../partials/statistics.html?raw';

// 面板的装配与数据流：控件回调直接抓出来调用，回包直接交给捕获的 host 监听器，
// 渲染结果落在 stub DOM 上按文本与 class 断言（与 input.test.ts 同一套路数）。
const hostHandlers = vi.hoisted(() => new Map<string, (message: { data: unknown }) => void>());
const toggleHandlers = vi.hoisted(() => new Map<string, (active: boolean) => void>());
const updateConfig = vi.hoisted(() => vi.fn());

vi.mock('../utils/host-messages', () => ({
  onHostMessage: (type: string, handler: (message: { data: unknown }) => void) => {
    hostHandlers.set(type, handler);
    return () => { };
  }
}));
vi.mock('./config-sync', () => ({ updateConfig }));
vi.mock('./shared', () => ({
  applyToggleState: vi.fn(),
  setupToggleButton: (id: string, onChanged: (active: boolean) => void) => { toggleHandlers.set(id, onChanged); }
}));

import { applyToggleState } from './shared';
import { dayKeyFromDate, formatDayKey } from '../utils/statistics-metrics';
import { applyStatisticsConfig, setupStatistics } from './statistics';

class StubElement {
  textContent = '';
  title = '';
  className = '';
  value = '';
  children: StubElement[] = [];
  style: Record<string, string> = {};
  private classes = new Set<string>();
  private listeners = new Map<string, Array<() => void>>();
  classList = {
    add: (name: string) => { this.classes.add(name); },
    remove: (name: string) => { this.classes.delete(name); },
    contains: (name: string) => this.classes.has(name),
    toggle: (name: string, force?: boolean) => {
      const next = force ?? !this.classes.has(name);
      if (next) this.classes.add(name);
      else this.classes.delete(name);
    }
  };
  appendChild(child: StubElement) { this.children.push(child); }
  append(...nodes: StubElement[]) { this.children.push(...nodes); }
  replaceChildren(...nodes: StubElement[]) { this.children = nodes; }
  addEventListener(type: string, handler: () => void) {
    const handlers = this.listeners.get(type) ?? [];
    handlers.push(handler);
    this.listeners.set(type, handlers);
  }
  dispatch(type: string) { (this.listeners.get(type) ?? []).forEach((handler) => handler()); }
}

const elements = new Map<string, StubElement>();
const windowListeners = new Map<string, Array<(event: unknown) => void>>();
let postMessage: ReturnType<typeof vi.fn>;

function element(id: string): StubElement {
  let node = elements.get(id);
  if (!node) {
    node = new StubElement();
    elements.set(id, node);
  }
  return node;
}

function lastRequest(): { type: string; data: { requestId: string; action: string } } {
  return JSON.parse(postMessage.mock.calls.at(-1)![0] as string);
}

function respond(data: Record<string, unknown>): void {
  hostHandlers.get('statsResponse')!({ type: 'statsResponse', data } as unknown as { data: unknown });
}

function dailyResponse(requestId: string): Record<string, unknown> {
  // 用真实的今天生成数据：面板的「今日」按本机日期判定，写死日期会在第二天开始假失败。
  const day = dayKeyFromDate(new Date());
  return {
    requestId,
    ok: true,
    daily: [{ day, cjk: 100, latin: 20, digit: 3, punct: 4, other: 1, activeMs: 120_000 }],
    hourly: [{ day, hour: 9, chars: 128, activeMs: 120_000 }],
    meta: { firstDay: day, enabled: true }
  };
}

beforeEach(() => {
  elements.clear();
  windowListeners.clear();
  hostHandlers.clear();
  toggleHandlers.clear();
  vi.clearAllMocks();
  postMessage = vi.fn();
  vi.stubGlobal('window', {
    chrome: { webview: { postMessage } },
    addEventListener: (type: string, handler: (event: unknown) => void) => {
      const handlers = windowListeners.get(type) ?? [];
      handlers.push(handler);
      windowListeners.set(type, handlers);
    }
  });
  vi.stubGlobal('document', {
    getElementById: (id: string) => element(id),
    createElement: () => new StubElement()
  });
});

afterEach(() => vi.unstubAllGlobals());

it('asks for the current statistics when the panel is assembled', () => {
  setupStatistics();
  expect(postMessage).toHaveBeenCalledTimes(1);
  expect(lastRequest().type).toBe('statsRequest');
  expect(lastRequest().data.action).toBe('query');
});

it('writes the master switch to config and shows the disabled hint', () => {
  setupStatistics();
  toggleHandlers.get('statisticsToggleBtn')!(false);
  expect(updateConfig).toHaveBeenCalledWith('statistics.enabled', false);
  expect(element('statisticsDisabledHint').classList.contains('is-hidden')).toBe(false);
  toggleHandlers.get('statisticsToggleBtn')!(true);
  expect(updateConfig).toHaveBeenLastCalledWith('statistics.enabled', true);
  expect(element('statisticsDisabledHint').classList.contains('is-hidden')).toBe(true);
});

it('accepts only booleans for the master switch from the config snapshot', () => {
  applyStatisticsConfig('false');
  expect(applyToggleState).not.toHaveBeenCalled();
  applyStatisticsConfig(false);
  expect(applyToggleState).toHaveBeenCalledWith('statisticsToggleBtn', false);
});

it('fills the retention dropdown only with a known window', () => {
  setupStatistics();
  applyStatisticsConfig(true, '90d');
  expect(element('statisticsRetention').value).toBe('90d');
  // 非白名单值/类型不污染下拉：非法值跳过，已有选择保持不动。
  applyStatisticsConfig(true, '7d');
  expect(element('statisticsRetention').value).toBe('90d');
  applyStatisticsConfig(true, 42);
  expect(element('statisticsRetention').value).toBe('90d');
});

it('renders cards and details from a response', () => {
  setupStatistics();
  const requestId = lastRequest().data.requestId;
  respond(dailyResponse(requestId));

  expect(element('statisticsData').classList.contains('is-hidden')).toBe(false);
  expect(element('statisticsEmpty').classList.contains('is-hidden')).toBe(true);
  expect(element('statisticsTodayChars').textContent).toBe('128 字');
  expect(element('statisticsTotalChars').textContent).toBe('128 字');
  expect(element('statisticsAverageChars').textContent).toBe('128 字');
  expect(element('statisticsTotalSub').textContent).toBe('1 天有输入记录');
  expect(element('statisticsDetails').children).toHaveLength(6);
  expect(element('statisticsDetails').children[0].children[1].textContent).toBe(
    `128 字 · ${formatDayKey(dayKeyFromDate(new Date()))}`
  );
  expect(element('statisticsHourly').children).toHaveLength(24);
});

it('falls back to the empty state without meta.firstDay', () => {
  setupStatistics();
  const requestId = lastRequest().data.requestId;
  respond({ requestId, ok: true, daily: [], hourly: [], meta: { enabled: false } });

  expect(element('statisticsEmptyTitle').textContent).toBe('还没有统计数据');
  expect(element('statisticsData').classList.contains('is-hidden')).toBe(true);
  expect(element('statisticsEmpty').classList.contains('is-hidden')).toBe(false);
  expect(applyToggleState).toHaveBeenCalledWith('statisticsToggleBtn', false);
});

it('sets the retention policy through config instead of posting a clear request', () => {
  setupStatistics();
  element('statisticsRetention').value = '30d';
  element('statisticsRetention').dispatch('change');

  expect(updateConfig).toHaveBeenCalledWith('statistics.retention', '30d');
  // 下拉只设策略：没有额外的 statsRequest，清理由 Server 在收到配置后执行。
  expect(postMessage).toHaveBeenCalledTimes(1);
  expect(lastRequest().data.action).toBe('query');
});

it('keeps forever selectable as a policy', () => {
  setupStatistics();
  element('statisticsRetention').value = 'forever';
  element('statisticsRetention').dispatch('change');
  // 「永久保留」是合法策略，照样写入配置；它不触发删除。
  expect(updateConfig).toHaveBeenCalledWith('statistics.retention', 'forever');
});

it('ignores a dropdown change that is not a known window', () => {
  setupStatistics();
  element('statisticsRetention').value = '7d';
  element('statisticsRetention').dispatch('change');
  expect(updateConfig).not.toHaveBeenCalled();
});

it('has no manual clear button left in the panel', () => {
  // 静态守卫：按钮与状态提示已经删掉，断言源文件里也没有，防止以后被重新加回来。
  expect(statisticsHtml).toContain('statisticsRetention');
  expect(statisticsHtml).not.toContain('statisticsClearButton');
  expect(statisticsHtml).not.toContain('statisticsClearStatus');
  expect(statisticsHtml).not.toContain('<button');
});

it('refreshes when the panel is shown again and drops superseded responses', () => {
  setupStatistics();
  const firstRequest = lastRequest().data.requestId;

  windowListeners.get('msime:module-shown')!.forEach((handler) => handler({ detail: { module: 'statistics' } }));
  expect(postMessage).toHaveBeenCalledTimes(2);
  const secondRequest = lastRequest().data.requestId;
  expect(secondRequest).not.toBe(firstRequest);

  respond({ requestId: firstRequest, ok: true, daily: [], hourly: [], meta: { enabled: true } });
  expect(element('statisticsEmptyTitle').textContent).toBe('');

  respond(dailyResponse(secondRequest));
  expect(element('statisticsTodayChars').textContent).toBe('128 字');
});
