/// <reference types="node" />
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { expect, it, vi } from 'vitest';
import { applyCaretStateIndicatorPosition, setupFloatingToolbar } from './floating-toolbar';
import { setupDropdownMenu, applyDropdownValue } from './shared';
import partial from '../partials/floating-toolbar.html?raw';

vi.mock('./shared', () => ({ setupDropdownMenu: vi.fn(), applyDropdownValue: vi.fn(), setupToggleButton: vi.fn() }));
vi.mock('./appearance', () => ({ syncCaretStateIndicatorPreview: vi.fn() }));
vi.mock('./skin', () => ({ syncAppearancePreviews: vi.fn() }));

const styles = readFileSync(fileURLToPath(new URL('../styles/modules/floating-toolbar.css', import.meta.url)), 'utf8');

it('keeps toolbar and caret controls in separate cards with separate previews', () => {
  const toolbarCard = partial.match(/<div class="section floating-toolbar-card">([\s\S]*?)<div class="section caret-state-indicator-card">/)?.[1] ?? '';
  const caretCard = partial.match(/<div class="section caret-state-indicator-card">([\s\S]*?)<div class="section floating-toolbar-appearance">/)?.[1] ?? '';

  expect(toolbarCard).toContain('id="ftbToggleBtn"');
  expect(toolbarCard).toContain('id="ftbPreviewHost"');
  expect(toolbarCard).not.toContain('caretStateIndicator');
  expect(caretCard).toContain('class="ftb-toggle-btn" id="caretStateIndicatorToggleBtn" role="switch" aria-label="显示光标状态提示" aria-checked="false"');
  expect(caretCard).toContain('aria-label="显示光标状态提示"');
  expect(caretCard).toContain('可与悬浮工具栏同时开启');
  expect(caretCard).not.toContain('关闭悬浮工具栏后');
  expect(caretCard).toContain('id="caretStateIndicatorPositionBtn"');
  expect(caretCard).toContain('id="caretStatePreviewHost"');
  expect(caretCard).not.toContain('id="ftbPreviewHost"');
  expect(caretCard).toContain('role="img" aria-label="光标状态提示预览：每个文字光标左上方分别显示中、中文标点和中文模式、全角、简体"');
  expect(caretCard).toContain('class="cand-preview" aria-hidden="true"');
});

it('shows the four runtime samples and preserves fixed punctuation slots', () => {
  const preview = partial.match(/<div class="candidate caret-state-preview-host"[\s\S]*?<\/div>\s*<\/div>\s*<\/div>/)?.[0] ?? '';
  expect(preview).toContain('>中</div>');
  expect(preview).toContain('aria-label="，。  中"');
  expect(preview).toContain('>全</div>');
  expect(preview).toContain('>简</div>');
  expect(preview.match(/class="caret-state-badge(?: |")/g)).toHaveLength(4);
  expect(preview.match(/class="caret-state-preview-item"[^>]*>\s*<div class="caret-state-badge[^>]*>[\s\S]*?<\/div>\s*<span class="caret-state-preview-caret"><\/span>\s*<\/div>/g)).toHaveLength(4);
  expect(preview).toContain('data-position="top-left"');
  expect(styles).toMatch(/\.caret-state-badge\s*\{[^}]*width:\s*30px;[^}]*height:\s*30px;/);
  expect(styles).toMatch(/\.caret-state-badge-punctuation\s*\{[^}]*display:\s*block;[^}]*width:\s*94px;/);
  expect(styles).toMatch(/\.caret-state-badge-punctuation\s*\{\s*--badge-width:\s*94px;/);
  expect(styles).toMatch(/\.caret-state-punctuation-slot\s*\{[^}]*left:\s*-1px;[^}]*width:\s*64px;/);
  expect(styles).toMatch(/\.caret-state-mode-slot\s*\{[^}]*right:\s*-1px;[^}]*width:\s*30px;/);
  expect(styles).toMatch(/\.caret-state-preview-host\s*\{[^}]*font-size:\s*20px;/);
  expect(styles).toMatch(/\.caret-state-preview-host\s*\{[^}]*grid-template-columns:\s*repeat\(4, 132px\);/);
  expect(styles).toMatch(/@container\s*\(max-width:\s*569px\)\s*\{\s*\.caret-state-preview-host\s*\{[^}]*grid-template-columns:\s*repeat\(2, 132px\);/);
  expect(styles).toMatch(/\.caret-state-preview-item\s*\{[^}]*width:\s*132px;[^}]*height:\s*132px;[^}]*border:/);
  expect(styles).toMatch(/\.caret-state-preview-caret\s*\{[^}]*width:\s*2px;[^}]*height:\s*24px;/);
  expect(styles).toMatch(/\.caret-state-badge\s*\{[^}]*top:\s*23px;[^}]*right:\s*29px;/);
  expect(styles).toMatch(/\[data-position="top"\] \.caret-state-badge/);
  expect(styles).toMatch(/\[data-position="top-right"\] \.caret-state-badge/);
  expect(styles).toMatch(/\[data-position="bottom"\] \.caret-state-badge\s*\{\s*top:\s*89px;/);
});

it('applies each selector choice immediately to all samples and also follows config snapshots', () => {
  const label = { setAttribute: vi.fn() };
  const host = { dataset: { position: 'top-left' }, closest: () => label };
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'caretStatePreviewHost' ? host : null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    const call = vi.mocked(setupDropdownMenu).mock.calls.find(([button]) => button === 'caretStateIndicatorPositionBtn');
    expect(call?.[4]).toBe('general.caret_state_indicator_position');
    const change = call?.[5];
    for (const [position, direction] of [
      ['top-left', '左上方'], ['top', '正上方'], ['top-right', '右上方'], ['bottom', '下方']
    ]) {
      expect(change?.(position)).toBe(position);
      expect(host.dataset.position).toBe(position);
      expect(label.setAttribute).toHaveBeenLastCalledWith('aria-label', expect.stringContaining(`每个文字光标${direction}`));
      applyCaretStateIndicatorPosition(position);
      expect(applyDropdownValue).toHaveBeenLastCalledWith('caretStateIndicatorPositionBtn', 'caretStateIndicatorPositionMenu', position);
      expect(host.dataset.position).toBe(position);
    }
  } finally {
    vi.unstubAllGlobals();
  }
});
