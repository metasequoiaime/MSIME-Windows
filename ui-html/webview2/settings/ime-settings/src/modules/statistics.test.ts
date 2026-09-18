import { afterEach, beforeEach, expect, it, vi } from 'vitest';

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
let confirm: ReturnType<typeof vi.fn>;

function element(id: string): StubElement {
  let node = elements.get(id);
  if (!node) {
    node = new StubElement();
    elements.set(id, node);
  }
  return node;
}

function lastRequest(): { type: string; data: { requestId: string; action: string; range?: string } } {
  return JSON.parse(postMessage.mock.calls.at(-1)![0] as string);
}

function respond(data: Record<string, unknown>): void {
  hostHandlers.get('statsResponse')!({ type: 'statsResponse', data } as unknown as { data: unknown });
}

function dailyResponse(requestId: string, day: number): Record<string, unknown> {
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
  confirm = vi.fn(() => true);
  vi.stubGlobal('window', {
    chrome: { webview: { postMessage } },
    confirm,
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

it('accepts only booleans from the config snapshot', () => {
  applyStatisticsConfig('false');
  expect(applyToggleState).not.toHaveBeenCalled();
  applyStatisticsConfig(false);
  expect(applyToggleState).toHaveBeenCalledWith('statisticsToggleBtn', false);
});

it('renders cards and details from a response', () => {
  setupStatistics();
  const requestId = lastRequest().data.requestId;
  respond(dailyResponse(requestId, 20260918));

  expect(element('statisticsData').classList.contains('is-hidden')).toBe(false);
  expect(element('statisticsEmpty').classList.contains('is-hidden')).toBe(true);
  expect(element('statisticsTodayChars').textContent).toBe('128 字');
  expect(element('statisticsTotalChars').textContent).toBe('128 字');
  expect(element('statisticsAverageChars').textContent).toBe('128 字');
  expect(element('statisticsTotalSub').textContent).toBe('1 天有输入记录');
  expect(element('statisticsDetails').children).toHaveLength(6);
  expect(element('statisticsDetails').children[0].children[1].textContent).toBe('128 字 · 2026-09-18');
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

it('clears history for the selected range and refreshes from the response', () => {
  setupStatistics();
  element('statisticsClearRange').value = '30d';
  element('statisticsClearButton').dispatch('click');

  expect(confirm).toHaveBeenCalledTimes(1);
  const clearRequest = lastRequest();
  expect(clearRequest.data.action).toBe('clear');
  expect(clearRequest.data.range).toBe('30d');

  respond(dailyResponse(clearRequest.data.requestId, 20260918));
  expect(element('statisticsClearStatus').textContent).toContain('已清除 30 天前的历史数据');
  expect(element('statisticsClearStatus').classList.contains('is-error')).toBe(false);
});

it('keeps the result of an unconfirmed clear untouched', () => {
  confirm.mockReturnValue(false);
  setupStatistics();
  element('statisticsClearRange').value = 'all';
  element('statisticsClearButton').dispatch('click');
  expect(postMessage).toHaveBeenCalledTimes(1);
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

  respond(dailyResponse(secondRequest, 20260918));
  expect(element('statisticsTodayChars').textContent).toBe('128 字');
});
