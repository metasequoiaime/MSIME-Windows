import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyAppearanceConfig, onCandidateSurfaceThemeChanged, setupAppearance } from './appearance';

const transforms = vi.hoisted(() => new Map<string, (value: string) => string>());
vi.mock('./shared', () => ({
  applyDropdownValue: vi.fn(), applyToggleState: vi.fn(), populateDropdownMenu: vi.fn(),
  registerDropdownPreparer: vi.fn(), setupToggleButton: vi.fn(),
  setupDropdownMenu: (_button: string, menu: string, _action: string, _stop: boolean,
                      _path: string, transform: (value: string) => string) => transforms.set(menu, transform)
}));
vi.mock('./theme', () => ({ applyThemeConfig: vi.fn(), setCandidateSurfaceThemeListener: vi.fn() }));
vi.mock('../utils/common-utils', () => ({ loadHTML: async () => '' }));

class Element extends EventTarget {
  children: Element[] = [];
  textContent = '';
  value = '';
  dataset: Record<string, string> = {};
  id = '';
  className = '';
  disabled = false;
  hidden = false;
  parent?: Element;
  style = { setProperty: vi.fn(), removeProperty: vi.fn() };
  classList = { toggle: vi.fn(), add: vi.fn(), remove: vi.fn() };
  appendChild(child: Element) { child.parent = this; this.children.push(child); }
  replaceChildren() { this.children = []; }
  setAttribute() {}
  querySelector(selector: string): Element | undefined { return this.querySelectorAll(selector)[0]; }
  querySelectorAll(selector: string): Element[] { return this.children.filter((child) => child.className.split(' ').includes(selector.slice(1))); }
  remove() { if (this.parent) this.parent.children = this.parent.children.filter((child) => child !== this); }
  select() {}
  focus() {}
}

let list: Element;
let preview: Element;
let caretPreview: Element;
let postMessage: ReturnType<typeof vi.fn>;
beforeEach(() => {
  list = new Element();
  preview = new Element();
  caretPreview = new Element();
  caretPreview.id = 'caretStatePreviewHost';
  postMessage = vi.fn();
  vi.stubGlobal('window', { chrome: { webview: { postMessage } } });
  vi.stubGlobal('document', {
    getElementById: (id: string) => {
      const find = (element: Element): Element | undefined => element.id === id ? element : element.children.map(find).find(Boolean);
      return id === 'candFallbackFontList' ? list : id.startsWith('candidate-wnd-') ? preview
        : id === 'caretStatePreviewHost' ? caretPreview : find(list);
    },
    addEventListener: vi.fn(),
    querySelector: () => null,
    querySelectorAll: (selector: string) => selector.startsWith('.cand-preview .candidate') ? [preview] : [],
    createElement: () => new Element()
  });
});
afterEach(() => vi.unstubAllGlobals());

function selects() { return list.children.filter((element) => element.className === 'dropdown fallback-font-dropdown').map((element) => element.children[0]); }
function labels() { return selects().map((button) => button.children[0].value).filter(Boolean); }
function choose(index: number, value: string) { return transforms.get(`fallbackFontMenu${index}`)!(value); }
function family() { return (preview.style as unknown as { fontFamily: string }).fontFamily; }

it('shows ordered selectors separated by fullwidth commas and saves replacement/removal', () => {
  applyAppearanceConfig(undefined, undefined, undefined, {
    english_font: 'Segoe UI', fallback_fonts: ['Font A', 'Font B'],
    fallback_font_css_families: ['Family A', 'Family B']
  });
  expect(family()).toBe('"Segoe UI", "Family A", "Family B", sans-serif');
  expect(list.children[1].textContent).toBe('，');
  const saved = choose(1, 'Font C');
  expect(labels()).toEqual(['Font A', 'Font C']);
  expect(family()).toBe('"Segoe UI", "Family A", "Font C", sans-serif');
  expect(saved).toBe('["Font A","Font C"]');
  choose(0, '');
  expect(labels()).toEqual(['Font C']);
});

it('keeps an explicit empty list and quotes punctuation inside font names', () => {
  applyAppearanceConfig(undefined, undefined, undefined, {
    english_font: 'Segoe UI', fallback_fonts: ['Font,"x\\']
  });
  expect(family()).toBe('"Segoe UI", "Font,\\"x\\\\", sans-serif');
  applyAppearanceConfig(undefined, undefined, undefined, { fallback_fonts: [] });
  expect(labels()).toEqual([]);
  expect(family()).toBe('"Segoe UI", sans-serif');
});

it('adds one empty selector with the plus button and only saves after choosing a font', async () => {
  await setupAppearance();
  applyAppearanceConfig(undefined, undefined, undefined, { fallback_fonts: ['SimSun'] });
  list.children.at(-1)!.dispatchEvent(new Event('click'));
  expect(selects().map((button) => button.children[0].value)).toEqual(['SimSun', '']);
  expect(selects()[0].className).toBe('dropdown-toggle font-combobox fallback-font-select');
  expect(list.children.at(-1)!.className).toBe('dropdown-toggle fallback-font-add');
  expect(postMessage).not.toHaveBeenCalled();
  const saved = choose(1, 'DengXian');
  expect(labels()).toEqual(['SimSun', 'DengXian']);
  expect(saved).toBe('["SimSun","DengXian"]');
});

it('keeps the caret preview on the resolved candidate theme and text color', () => {
  applyAppearanceConfig(undefined, undefined, undefined, { cand_text_color: '#123456' });
  expect(caretPreview.style.setProperty).toHaveBeenCalledWith('--caret-text-override', '#123456');

  onCandidateSurfaceThemeChanged('light');
  expect(caretPreview.classList.toggle).toHaveBeenCalledWith('theme-light', true);
  expect(caretPreview.classList.toggle).toHaveBeenCalledWith('theme-dark', false);

  applyAppearanceConfig(undefined, undefined, undefined, { cand_text_color: 'auto' });
  expect(caretPreview.style.removeProperty).toHaveBeenCalledWith('--caret-text-override');
});

it('filters each supplementary font menu independently without persisting search text', () => {
  applyAppearanceConfig(undefined, undefined, undefined, {
    fallback_fonts: ['SimSun', 'DengXian'], system_fonts: ['SimSun', 'SimHei', 'DengXian']
  });
  const input = selects()[0].children[0];
  input.value = 'sIM';
  input.dispatchEvent(new Event('input'));
  const dropdowns = list.children.filter((item) => item.className === 'dropdown fallback-font-dropdown');
  const menu = dropdowns[0].children[1];
  expect(menu.children.filter((item) => !item.hidden).map((item) => item.textContent)).toEqual(['SimSun', 'SimHei']);
  expect(dropdowns[1].children[1].children.every((item) => !item.hidden)).toBe(true);
  expect(postMessage).not.toHaveBeenCalled();
  input.value = 'no such font';
  input.dispatchEvent(new Event('input'));
  expect(menu.children.at(-1)!.textContent).toBe('没有匹配的字体');
  input.value = '';
  input.dispatchEvent(new Event('input'));
  expect(menu.children.map((item) => item.textContent)).toEqual(['移除此字体', 'SimSun', 'SimHei', 'DengXian']);
});
