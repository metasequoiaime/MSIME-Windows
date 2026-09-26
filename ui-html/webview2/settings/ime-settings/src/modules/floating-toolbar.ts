import { applyDropdownValue, setupDropdownMenu, setupToggleButton } from './shared';
import { syncCaretStateIndicatorPreview } from './appearance';
import { updateConfig } from './config-sync';
import { syncAppearancePreviews } from './skin';
import ftbHTML from '../../../../ftb/default.html?raw';

type FloatingToolbarItem = 'fullwidth' | 'punctuation' | 'character_set' | 'emoji' | 'screen_keyboard' | 'settings';
type FloatingToolbarItemsConfig = Partial<Record<FloatingToolbarItem, boolean>>;

const toolbarItems: FloatingToolbarItem[] = [
  'fullwidth',
  'punctuation',
  'character_set',
  'emoji',
  'screen_keyboard',
  'settings'
];

const toolbarItemState: Record<FloatingToolbarItem, boolean> = {
  fullwidth: true,
  punctuation: true,
  character_set: true,
  emoji: true,
  screen_keyboard: false,
  settings: true
};

let toolbarScale = 1;
let toolbarFontSize = 24;

export function setupFloatingToolbar(): void {
  mountFloatingToolbarPreview();
  syncCaretStateIndicatorPreview();
  syncAppearancePreviews();

  setupToggleButton('ftbToggleBtn', (active) => {
    updateConfig('general.floating_toolbar', active);
    document.getElementById('ftbToggleBtn')?.setAttribute('aria-checked', String(active));
  });

  setupToggleButton('caretStateIndicatorToggleBtn', (active) => {
    updateConfig('general.caret_state_indicator', active);
    document.getElementById('caretStateIndicatorToggleBtn')?.setAttribute('aria-checked', String(active));
  });

  setupDropdownMenu('caretStateIndicatorPositionBtn', 'caretStateIndicatorPositionMenu', '', true,
    'general.caret_state_indicator_position', (value) => {
      updateCaretPreviewPosition(value);
      return value;
    });

  setupDropdownMenu('ftbScaleBtn', 'ftbScaleMenu', '', true, 'general.floating_toolbar_scale', (value) => {
    const parsed = Number(value);
    toolbarScale = Number.isFinite(parsed) && parsed > 0 ? parsed : 1;
    applyPreviewAppearance();
    return toolbarScale;
  });

  setupDropdownMenu('ftbFontSizeBtn', 'ftbFontSizeMenu', '', true, 'general.floating_toolbar_font_size', (value) => {
    const parsed = Number(value);
    toolbarFontSize = Number.isFinite(parsed) ? parsed : 24;
    applyPreviewAppearance();
    return toolbarFontSize;
  });

  document.querySelectorAll<HTMLInputElement>('.floating-toolbar-component-list input[data-toolbar-item]')
    .forEach((checkbox) => {
      checkbox.addEventListener('change', () => {
        const item = checkbox.dataset.toolbarItem as FloatingToolbarItem;
        toolbarItemState[item] = checkbox.checked;
        updatePreviewItems();
        updateConfig(`general.floating_toolbar_${item}`, checkbox.checked);
      });
    });
}

export function applyFloatingToolbarItemsConfig(config: FloatingToolbarItemsConfig | undefined): void {
  if (!config) return;
  toolbarItems.forEach((item) => {
    if (typeof config[item] !== 'boolean') return;
    toolbarItemState[item] = config[item]!;
    const checkbox = document.querySelector<HTMLInputElement>(
      `.floating-toolbar-component-list input[data-toolbar-item="${item}"]`
    );
    if (checkbox) checkbox.checked = toolbarItemState[item];
  });
  updatePreviewItems();
}

export function applyCaretStateIndicatorPosition(position?: string): void {
  const value = position || 'top-left';
  applyDropdownValue('caretStateIndicatorPositionBtn', 'caretStateIndicatorPositionMenu', value);
  updateCaretPreviewPosition(value);
}

function updateCaretPreviewPosition(position: string): void {
  const host = document.getElementById('caretStatePreviewHost');
  if (!host) return;
  host.dataset.position = position;
  const direction = ({ 'top-left': '左上方', top: '正上方', 'top-right': '右上方', bottom: '下方' } as Record<string, string>)[position];
  host.closest('.caret-state-preview')?.setAttribute('aria-label',
    `光标状态提示预览：每个文字光标${direction || '左上方'}分别显示中、中文标点和中文模式、全角、简体`);
}

export function applyFloatingToolbarAppearanceConfig(scale?: number, fontSize?: number): void {
  if (typeof scale === 'number' && scale > 0) {
    toolbarScale = scale;
    applyDropdownValue('ftbScaleBtn', 'ftbScaleMenu', normalizeScaleKey(scale));
  }
  if (typeof fontSize === 'number' && fontSize > 0) {
    toolbarFontSize = fontSize;
    applyDropdownValue('ftbFontSizeBtn', 'ftbFontSizeMenu', String(fontSize));
  }
  applyPreviewAppearance();
}

function normalizeScaleKey(scale: number): string {
  if (Math.abs(scale - 0.75) < 0.001) return '0.75';
  if (Math.abs(scale - 1.25) < 0.001) return '1.25';
  if (Math.abs(scale - 1.5) < 0.001) return '1.5';
  return '1';
}

function mountFloatingToolbarPreview(): void {
  const host = document.getElementById('ftbPreviewHost');
  if (!host) return;

  const source = new DOMParser().parseFromString(ftbHTML, 'text/html');
  const statusBar = source.querySelector<HTMLElement>('.status-bar');
  if (!statusBar) return;

  statusBar.querySelectorAll('#en, #fullwidth, #puncEn').forEach((element) => element.remove());
  statusBar.querySelectorAll<HTMLElement>('[id]').forEach((element) => element.removeAttribute('id'));
  host.replaceChildren(statusBar);
  updatePreviewItems();
  applyPreviewAppearance();
}

function updatePreviewItems(): void {
  const host = document.getElementById('ftbPreviewHost');
  if (!host) return;
  toolbarItems.forEach((item) => {
    const element = host.querySelector<HTMLElement>(`[data-toolbar-item="${item}"]`);
    if (element) element.style.display = toolbarItemState[item] ? 'flex' : 'none';
  });
}

function applyPreviewAppearance(): void {
  const host = document.getElementById('ftbPreviewHost');
  if (!host) return;
  host.style.setProperty('--ftb-scale', String(toolbarScale));
  host.style.setProperty('--ftb-icon-size', `${toolbarFontSize}px`);
}
