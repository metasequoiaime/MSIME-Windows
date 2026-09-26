import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyCandidateSkin, applyCandidateSkinCatalog } from './skin';

class PreviewElement {
  dataset: Record<string, string> = {};
  style = { setProperty: vi.fn(), removeProperty: vi.fn() };
  classList = { toggle: vi.fn(), contains: (name: string) => name === 'caret-state-preview-host' };
  querySelector() { return null; }
}

let preview: PreviewElement;
let generatedCss = '';

beforeEach(() => {
  preview = new PreviewElement();
  generatedCss = '';
  const styles = new Map<string, { id: string }>();
  class Sheet {
    cssRules: Array<{ cssText?: string; cssRules?: unknown[]; insertRule?: (rule: string) => void }> = [];
    deleteRule(index: number) { this.cssRules.splice(index, 1); }
    insertRule(rule: string) {
      if (rule.startsWith('@scope')) {
        this.cssRules.push({ cssRules: [], insertRule: () => undefined });
      } else {
        this.cssRules.push({ cssText: rule });
      }
    }
    replaceSync(css: string) {
      generatedCss = css;
      this.cssRules = [{ cssText: css }];
    }
  }
  vi.stubGlobal('CSSStyleSheet', Sheet);
  vi.stubGlobal('document', {
    querySelectorAll: (selector: string) => selector === '.cand-preview .candidate' ? [preview] : [],
    getElementById: (id: string) => styles.get(id) ?? null,
    createElement: () => ({ id: '', dataset: {}, sheet: new Sheet() }),
    head: { appendChild: (style: { id: string }) => styles.set(style.id, style) }
  });
});

afterEach(() => vi.unstubAllGlobals());

it('applies built-in and custom candidate skins to the caret preview consumer', () => {
  applyCandidateSkin('wechat');
  expect(preview.classList.toggle).toHaveBeenCalledWith('skin-wechat', true);

  applyCandidateSkin('autumn_osmanthus');
  expect(preview.classList.toggle).toHaveBeenCalledWith('skin-autumn-osmanthus', true);
  expect(preview.classList.toggle).toHaveBeenCalledWith('skin-wechat', false);

  applyCandidateSkinCatalog([
    {
      id: 'custom-blue', name: 'Custom Blue', version: '1', base: 'graphite', layouts: ['horizontal'],
      themes: ['dark', 'light'], compatible: true,
      candidate: {
        dark: { surface: '#102030', border: '#405060', text: '#708090' },
        light: { surface: '#f0f1f2', border: '#d0d1d2', text: '#202122' }
      }
    }
  ], [], '', true, 0);
  applyCandidateSkin('custom-blue');

  expect(preview.classList.toggle).toHaveBeenCalledWith('skin-graphite', true);
  expect(preview.dataset.externalCaretSkinPreview).toBe('custom-blue');
  expect(preview.dataset.externalSkinPreview).toBeUndefined();
  expect(generatedCss).toContain(':scope.candidate.theme-dark { --cand-bg: #102030; --cand-border: #405060; --cand-text: #708090; }');
  expect(generatedCss).toContain(':scope.caret-state-preview-host.theme-dark { --cand-bg: #102030; --cand-border: #405060; --cand-text: #708090; }');
  expect(generatedCss).toContain(':scope.candidate.theme-light { --cand-bg: #f0f1f2; --cand-border: #d0d1d2; --cand-text: #202122; }');
  expect(generatedCss).toContain(':scope.caret-state-preview-host.theme-light { --cand-bg: #f0f1f2; --cand-border: #d0d1d2; --cand-text: #202122; }');
});
