import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import inputHtml from '../partials/input.html?raw';

vi.mock('./theme', () => ({ setSurfaceTheme: vi.fn(), setThemeMode: vi.fn() }));
vi.mock('./appearance', () => ({ updateCandidatePreviewHelpcode: vi.fn() }));

// Use the shipped menu with the same small EventTarget DOM shim as shared.test.ts.
// The selection, host message validation and config snapshot handlers stay real.
class MenuElement extends EventTarget {
  textContent = '';
  dataset: Record<string, string> = {};
  classList = { remove: vi.fn() };
  label?: MenuElement;
  querySelector() { return this.label; }
  contains() { return true; }
  closest() { return this; }
  getAttribute() { return null; }
}

let menu: MenuElement;
let label: MenuElement;
let items: MenuElement[];
let webview: EventTarget;
let postMessage: ReturnType<typeof vi.fn>;

beforeEach(async () => {
  vi.resetModules();
  const markup = inputHtml.match(/id="shuangpinSchemeMenu">([\s\S]*?)\n\s*<\/div>\n/)?.[1];
  expect(markup).toBeDefined();
  items = Array.from(markup!.matchAll(/data-value="([^"]+)">([^<]+)<\/div>/g), ([, value, text]) => {
    const item = new MenuElement();
    item.dataset.value = value;
    item.textContent = text;
    return item;
  });
  const button = new MenuElement();
  label = new MenuElement();
  label.textContent = inputHtml.match(/id="shuangpinSchemeBtn">\s*<span>([^<]+)<\/span>/)![1];
  button.label = label;
  menu = new MenuElement();
  postMessage = vi.fn();
  webview = Object.assign(new EventTarget(), { postMessage });
  vi.stubGlobal('HTMLInputElement', class {});
  vi.stubGlobal('window', { chrome: { webview } });
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'shuangpinSchemeBtn' ? button
      : id === 'shuangpinSchemeMenu' || id === 'input' ? menu : null,
    querySelector: (selector: string) => selector === '#shuangpinSchemeBtn span, #shuangpinSchemeBtn input'
      ? label : null,
    querySelectorAll: (selector: string) => selector === '#shuangpinSchemeMenu .dropdown-item' ? items : [],
    addEventListener: vi.fn()
  });
  vi.spyOn(console, 'warn').mockImplementation(() => {});
  const { setupInput } = await import('./input');
  setupInput();
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

function snapshot(schema: string): void {
  const event = new Event('message');
  Object.defineProperty(event, 'data', { value: {
    type: 'configSnapshot',
    data: { input: { mode: 'chinese', schema: 'shuangpin', shuangpin_schema: schema } }
  } });
  webview.dispatchEvent(event);
}

it('offers Jiajia with the four existing schemes and keeps Xiaohe as the initial label', () => {
  expect(items.map(item => item.dataset.value)).toEqual(['xiaohe', 'ziranma', 'shoudao', 'microsoft', 'jiajia']);
  expect(label.textContent).toBe('小鹤双拼');
  expect(items.find(item => item.dataset.value === 'jiajia')?.textContent).toBe('拼音加加双拼');
});

it('sends the Jiajia selection through the existing config update contract', () => {
  const event = new Event('click');
  Object.defineProperty(event, 'target', { value: items.find(item => item.dataset.value === 'jiajia') });
  menu.dispatchEvent(event);
  expect(label.textContent).toBe('拼音加加双拼');
  expect(postMessage).toHaveBeenCalledTimes(1);
  expect(JSON.parse(postMessage.mock.calls[0][0])).toMatchObject({
    type: 'configUpdate', data: { path: 'input.shuangpin_schema', value: 'jiajia' }
  });
});

it('restores a saved Jiajia snapshot after the input page loads without writing it back', async () => {
  const { setupConfigSync, notifySettingsModuleReady } = await import('./config-sync');
  setupConfigSync();
  expect(JSON.parse(postMessage.mock.calls[0][0])).toMatchObject({ type: 'configRequest' });
  postMessage.mockClear();
  snapshot('jiajia');
  expect(label.textContent).toBe('小鹤双拼');
  notifySettingsModuleReady('input');
  await vi.waitFor(() => expect(label.textContent).toBe('拼音加加双拼'));
  expect(postMessage).not.toHaveBeenCalled();

  snapshot('microsoft');
  await vi.waitFor(() => expect(label.textContent).toBe('微软双拼'));
  snapshot('xiaohe');
  await vi.waitFor(() => expect(label.textContent).toBe('小鹤双拼'));
  expect(postMessage).not.toHaveBeenCalled();
});

it('ignores an older snapshot when a newer scheme arrives before the module import completes', async () => {
  const { setupConfigSync, notifySettingsModuleReady } = await import('./config-sync');
  setupConfigSync();
  notifySettingsModuleReady('input');
  postMessage.mockClear();
  snapshot('microsoft');
  snapshot('jiajia');
  await vi.waitFor(() => expect(label.textContent).toBe('拼音加加双拼'));
  expect(postMessage).not.toHaveBeenCalled();
});
