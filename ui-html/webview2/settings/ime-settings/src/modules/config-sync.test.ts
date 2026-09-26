import { afterEach, beforeEach, expect, it, vi } from 'vitest';

// 捕获 setupConfigSync 注册的 configSnapshot 处理器后直接喂快照，验证回填守卫。
const handlers = vi.hoisted(() => new Map<string, (payload: { data?: Record<string, unknown> }) => void>());

vi.mock('../utils/host-messages', () => ({
  onHostMessage: (type: string, handler: (payload: { data?: Record<string, unknown> }) => void) => {
    handlers.set(type, handler);
  }
}));
vi.mock('./shared', () => ({
  applyCandidateArrange: vi.fn(),
  applyDropdownValue: vi.fn(),
  applyToggleState: vi.fn(),
  setFuzzyRuleOptionsDisabled: vi.fn(),
  setSmartPunctuationOptionsDisabled: vi.fn()
}));
// The snapshot handler hands sections to these modules through fire-and-forget
// dynamic imports. This test covers only the backfill guard, so keep their DOM
// side effects out of it with no-op consumers.
vi.mock('./appearance', () => ({ applyAppearanceConfig: vi.fn(), updateCandidatePreviewHelpcode: vi.fn() }));
vi.mock('./skin', () => ({ applyCandidateSkin: vi.fn(), applyCandidateSkinCatalog: vi.fn() }));
vi.mock('./input', () => ({
  applyCustomTranslationConfig: vi.fn(),
  applyFrequencyConfig: vi.fn(),
  applyInputConfig: vi.fn(),
  applyNiuTransConfig: vi.fn(),
  applyTencentTmtConfig: vi.fn(),
  applyZhEnMixedInputConfig: vi.fn()
}));
vi.mock('./voice', () => ({ applyVoiceConfig: vi.fn() }));
vi.mock('./ai-settings', () => ({ applyAiConfig: vi.fn() }));
vi.mock('./floating-toolbar', () => ({
  applyCaretStateIndicatorPosition: vi.fn(),
  applyFloatingToolbarAppearanceConfig: vi.fn(),
  applyFloatingToolbarItemsConfig: vi.fn()
}));
vi.mock('./stats', () => ({ applyStatisticsEnabled: vi.fn(), applyStatisticsRetention: vi.fn() }));
vi.mock('./shortcut', () => ({ applyShortcutConfig: vi.fn() }));

import { applyToggleState } from './shared';
import { setupConfigSync } from './config-sync';

beforeEach(() => {
  handlers.clear();
  vi.clearAllMocks();
  vi.stubGlobal('document', {
    getElementById: () => ({}),
    querySelector: () => null,
    querySelectorAll: () => [],
    addEventListener: vi.fn()
  });
  vi.stubGlobal('window', { chrome: { webview: { postMessage: vi.fn() } } });
  setupConfigSync();
});

afterEach(async () => {
  // Let the snapshot's lazy imports and their callbacks finish while the
  // stubbed globals still exist; otherwise they can run after teardown.
  await vi.dynamicImportSettled();
  vi.unstubAllGlobals();
});

it('backfills the word-to-character switch only from a boolean', () => {
  const snapshot = handlers.get('configSnapshot')!;
  snapshot({ data: { input: { word_to_character: true } } });
  expect(applyToggleState).toHaveBeenCalledWith('wordToCharacterToggleBtn', true);
  snapshot({ data: { input: { word_to_character: false } } });
  expect(applyToggleState).toHaveBeenLastCalledWith('wordToCharacterToggleBtn', false);

  vi.mocked(applyToggleState).mockClear();
  snapshot({ data: { input: { word_to_character: 'yes' } } });
  expect(applyToggleState).not.toHaveBeenCalled();
});

it('backfills the focus announcement switch only from a boolean', () => {
  const snapshot = handlers.get('configSnapshot')!;
  snapshot({ data: { general: { caret_state_indicator_on_focus: true } } });
  expect(applyToggleState).toHaveBeenCalledWith('caretStateIndicatorOnFocusToggleBtn', true);
  snapshot({ data: { general: { caret_state_indicator_on_focus: false } } });
  expect(applyToggleState).toHaveBeenLastCalledWith('caretStateIndicatorOnFocusToggleBtn', false);

  vi.mocked(applyToggleState).mockClear();
  snapshot({ data: { general: {} } });
  expect(applyToggleState).not.toHaveBeenCalledWith('caretStateIndicatorOnFocusToggleBtn', expect.anything());
});
