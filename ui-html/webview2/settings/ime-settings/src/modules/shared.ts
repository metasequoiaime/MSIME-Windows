import { serializeHostMessage } from '../../../../shared/messages';
// 下拉菜单功能
import { setSurfaceTheme, setThemeMode } from './theme';

/** Deferred item building for menus that are too costly to keep rendered (system fonts). */
export type DropdownPreparer = {
  isPending: () => boolean;
  prepare: () => void;
};

const dropdownPreparers = new Map<string, DropdownPreparer>();

// Keeps the spinner on screen long enough to read instead of flashing for one frame.
const MIN_SPINNER_MS = 150;

export function registerDropdownPreparer(menuId: string, preparer: DropdownPreparer): void {
  dropdownPreparers.set(menuId, preparer);
}

// ---- octagram 语法模型（八股文）状态 ----

let collocationPollTimer: ReturnType<typeof setInterval> | null = null;

export type CollocationModelStatus = { state?: string; progress?: number; error?: string };
export type CollocationCatalogEntry = {
  id: string; displayName?: string; sizeHint?: string; license?: string; url?: string
};

// 下载与本地导入共用宿主的单槽：任一在进行时，其余行的下载/导入一并禁用，并持续轮询快照。
function isCollocationBusy(state: string | undefined): boolean {
  return state === 'downloading' || state === 'importing';
}

// 整句开关只有在当前激活的模型就绪时才有意义：模型缺席（包括未选择任何模型）时词格会
// 静默降级，开关开着也看不到任何效果。与智能标点／候选混输的子开关一样，这里只置灰
// 禁用，不替用户改回勾选状态——他开过但模型没了，得自己看见并决定。
const COLLABORATION_TOGGLE_IDS = ['sentenceCollocationToggleBtn'];

function setCollocationTogglesDisabled(disabled: boolean): void {
  for (const id of COLLABORATION_TOGGLE_IDS) {
    const toggle = document.getElementById(id);
    toggle?.setAttribute('aria-disabled', String(disabled));
    if (toggle) toggle.tabIndex = disabled ? -1 : 0;
  }
  document.querySelectorAll('.collocation-toggle-row').forEach((row) => {
    row.classList.toggle('is-disabled', disabled);
  });
}

function collocationStatusText(status: CollocationModelStatus | undefined): string {
  const state = status?.state ?? 'absent';
  if (state === 'ready') return '模型已就绪';
  if (state === 'downloading') return `下载中 ${status?.progress ?? 0}%`;
  if (state === 'importing') return '导入并校验中…';
  if (state === 'error') return `下载失败：${status?.error || '未知原因'}`;
  return '未下载';
}

// 按 catalog 播种模型列表行：显示名/大小/许可 + 状态文本 + 激活单选/下载/删除。每行都是
// 普通目录条目，激活单选写各自的 id（没有默认/回退行）；行内控件只发消息，状态一律等
// 下一份快照回放，页面不持本地状态机。
function seedCollocationRows(catalog: CollocationCatalogEntry[]): void {
  const list = document.getElementById('collocationModelList');
  // 行只播种一次，以容器非空为标志：2 秒轮询期间逐帧重建会把正在点击的按钮从焦点里
  // 换掉，而目录随 Server 版本固定，装载后不会变。
  if (!list || list.children.length > 0 || catalog.length === 0) return;
  for (const entry of catalog) {
    const row = document.createElement('div');
    row.className = 'collocation-model-row';
    row.dataset.modelId = entry.id;

    const info = document.createElement('div');
    info.className = 'collocation-model-info';
    const name = document.createElement('div');
    name.className = 'collocation-model-name';
    name.textContent = entry.displayName ?? entry.id;
    const meta = document.createElement('span');
    meta.className = 'collocation-model-meta';
    meta.textContent = [entry.sizeHint, entry.license].filter(Boolean).join(' · ');
    name.appendChild(meta);
    // 应用内下载慢时的备用通道：直链交给系统浏览器（宿主只放行 https://），下好后用「导入」。
    if (entry.url) {
      const url = entry.url;
      const browser = document.createElement('button');
      browser.type = 'button';
      browser.className = 'collocation-model-link';
      browser.textContent = '浏览器下载';
      browser.title = url;
      browser.addEventListener('click', () => {
        window.chrome?.webview?.postMessage(serializeHostMessage({ type: 'openExternalUrl', data: url }));
      });
      name.appendChild(browser);
    }
    const status = document.createElement('div');
    status.className = 'input-setting-description collocation-model-status';
    info.appendChild(name);
    info.appendChild(status);

    const actions = document.createElement('div');
    actions.className = 'collocation-model-actions';
    const radio = document.createElement('input');
    radio.type = 'radio';
    radio.name = 'collocation-model';
    radio.value = entry.id;
    radio.addEventListener('change', () => {
      window.chrome?.webview?.postMessage(serializeHostMessage({
        type: 'configUpdate',
        data: { path: 'association.sentence_collocation_model', value: radio.value }
      }));
    });
    actions.appendChild(radio);
    const download = document.createElement('button');
    download.type = 'button';
    download.className = 'restart-server-button collocation-model-download';
    download.textContent = '下载';
    download.addEventListener('click', () => {
      window.chrome?.webview?.postMessage(serializeHostMessage({
        type: 'collocationModelDownload', data: { modelId: entry.id }
      }));
    });
    actions.appendChild(download);
    const importButton = document.createElement('button');
    importButton.type = 'button';
    importButton.className = 'restart-server-button collocation-model-import';
    importButton.textContent = '导入';
    importButton.title = '选择用浏览器下载好的 .gram 文件';
    importButton.addEventListener('click', () => {
      window.chrome?.webview?.postMessage(serializeHostMessage({
        type: 'collocationModelImport', data: { modelId: entry.id }
      }));
    });
    actions.appendChild(importButton);
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'restart-server-button collocation-model-delete';
    remove.textContent = '删除';
    remove.addEventListener('click', () => {
      window.chrome?.webview?.postMessage(serializeHostMessage({
        type: 'collocationModelDelete', data: { modelId: entry.id }
      }));
    });
    actions.appendChild(remove);

    row.appendChild(info);
    row.appendChild(actions);
    list.appendChild(row);
  }
}

// 应用快照里的 association 模型集合：按 catalog 播种行（仅一次），逐行刷新状态文本与
// 控件可用态；整句开关按「当前激活的模型 ready」置灰（未选择任何模型时同样置灰）；任一
// 模型下载中每 2 秒发一次 collocationModelStatusRequest 拉新快照，全部离开下载态自停。
export function applyCollocationModelStatus(
  statuses: Record<string, CollocationModelStatus> | undefined,
  catalog: CollocationCatalogEntry[] | undefined,
  activeModel: string | undefined
): void {
  seedCollocationRows(catalog ?? []);
  // 激活值留空 = 未选择任何模型：没有受保护的包，整句开关保持置灰。
  const activeId = activeModel ?? '';
  // 单槽：同一时刻至多一个下载或导入在进行，宿主对忙碌请求直接拒绝。
  const anyBusy = Object.values(statuses ?? {}).some((s) => isCollocationBusy(s.state));
  document.querySelectorAll<HTMLElement>('.collocation-model-row').forEach((row) => {
    const id = row.dataset.modelId ?? '';
    const status = statuses?.[id];
    const state = status?.state ?? 'absent';
    const text = row.querySelector<HTMLElement>('.collocation-model-status');
    if (text) text.textContent = collocationStatusText(status);
    const download = row.querySelector<HTMLButtonElement>('.collocation-model-download');
    // 其余行的下载/导入按钮在忙碌态一并禁用，把单槽语义摆到界面上（服务端拒绝是兜底）。
    if (download) download.disabled = state === 'ready' || anyBusy;
    const importButton = row.querySelector<HTMLButtonElement>('.collocation-model-import');
    if (importButton) {
      importButton.hidden = state === 'ready';
      importButton.disabled = anyBusy;
    }
    const remove = row.querySelector<HTMLButtonElement>('.collocation-model-delete');
    if (remove) {
      // 任何就绪的包都可删（包括当前激活的包）：宿主删除激活包时会一并清空激活值，
      // 快照回放后该行回到未下载、未选中。
      remove.hidden = state !== 'ready';
    }
    const radio = row.querySelector<HTMLInputElement>('input[type="radio"]');
    if (radio) {
      // 只有就绪的包才可激活：激活一个没下载的包只会得到静默降级。
      radio.disabled = state !== 'ready';
      radio.checked = (activeModel ?? '') === radio.value;
    }
  });
  setCollocationTogglesDisabled(statuses?.[activeId]?.state !== 'ready');
  if (anyBusy && collocationPollTimer === null) {
    collocationPollTimer = setInterval(() => {
      window.chrome?.webview?.postMessage(serializeHostMessage({ type: 'collocationModelStatusRequest' }));
    }, 2000);
  } else if (!anyBusy && collocationPollTimer !== null) {
    clearInterval(collocationPollTimer);
    collocationPollTimer = null;
  }
}

function nextPaint(): Promise<void> {
  return new Promise((resolve) => {
    requestAnimationFrame(() => requestAnimationFrame(() => resolve()));
  });
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function openDropdownMenu(menu: HTMLElement, menuId: string): Promise<void> {
  const preparer = dropdownPreparers.get(menuId);
  if (!preparer?.isPending()) {
    menu.classList.add('open');
    return;
  }

  const spinner = document.createElement('div');
  spinner.className = 'dropdown-loading';
  spinner.setAttribute('role', 'status');
  spinner.setAttribute('aria-label', '正在加载');
  menu.appendChild(spinner);
  menu.classList.add('is-loading', 'open');

  const shownAt = performance.now();
  // Give the spinner a frame to paint before the blocking build starts; its
  // rotation is compositor driven so it keeps animating through the jank.
  await nextPaint();
  try {
    preparer.prepare();
  } finally {
    const remaining = MIN_SPINNER_MS - (performance.now() - shownAt);
    if (remaining > 0) {
      // prepare() rebuilds the menu children, which detaches the spinner.
      menu.appendChild(spinner);
      await delay(remaining);
    }
    spinner.remove();
    menu.classList.remove('is-loading');
  }
}

export function setupDropdownMenu(
  btnId: string,
  menuId: string,
  messageAction: string,
  useStopPropagation: boolean = false,
  configPath?: string,
  valueTransform: (value: string) => string | number | boolean = (value) => value,
  signal?: AbortSignal
): void {
  const btn = document.getElementById(btnId);
  const menu = document.getElementById(menuId);

  if (!btn || !menu) {
    console.warn(`Elements not found for ${btnId} or ${menuId}`);
    return;
  }

  const label = btn.querySelector<HTMLElement>('span, input');
  if (!label) {
    console.warn(`Label not found in ${btnId}`);
    return;
  }

  const enabledItems = (): HTMLElement[] =>
    Array.from(menu.querySelectorAll<HTMLElement>('.dropdown-item:not([aria-disabled="true"])'));

  // Moves focus one item in `direction`. Entering the menu from outside (the
  // toggle button still has focus) lands on the first item going down and on
  // the last item going up.
  const focusMenuItem = (direction: 1 | -1): void => {
    const items = enabledItems();
    if (items.length === 0) return;
    items.forEach((item) => {
      item.tabIndex = -1;
      item.setAttribute('role', 'option');
    });
    const current = items.indexOf(document.activeElement as HTMLElement);
    const next = current < 0
      ? (direction > 0 ? 0 : items.length - 1)
      : (current + direction + items.length) % items.length;
    items[next].focus();
  };

  btn.addEventListener('keydown', (event: KeyboardEvent) => {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
    event.preventDefault();
    const direction = event.key === 'ArrowDown' ? 1 : -1;
    void openDropdownMenu(menu, menuId).then(() => focusMenuItem(direction));
  }, { signal });

  menu.addEventListener('keydown', (event: KeyboardEvent) => {
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      focusMenuItem(event.key === 'ArrowDown' ? 1 : -1);
    } else if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      const focused = document.activeElement as HTMLElement | null;
      if (focused && enabledItems().includes(focused)) focused.click();
    } else if (event.key === 'Escape') {
      event.preventDefault();
      menu.classList.remove('open');
      btn.focus();
    }
  }, { signal });

  btn.addEventListener('click', (e: Event) => {
    if (useStopPropagation) {
      e.stopPropagation();
    }
    const clickedInput = (e.target as HTMLElement | null)?.matches('input');
    const willOpen = clickedInput || !menu.classList.contains('open');
    document.querySelectorAll('.dropdown-menu.open').forEach((openMenu) => {
      if (openMenu !== menu) {
        openMenu.classList.remove('open');
      }
    });
    if (willOpen) {
      void openDropdownMenu(menu, menuId);
    } else {
      menu.classList.remove('open');
    }
  }, { signal });

  // Event delegation so dynamically rebuilt items (e.g. system fonts) keep working.
  menu.addEventListener('click', (event: Event) => {
    const item = (event.target as HTMLElement | null)?.closest('.dropdown-item') as HTMLElement | null;
    if (!item || !menu.contains(item) || item.getAttribute('aria-disabled') === 'true') {
      return;
    }

    if (label instanceof HTMLInputElement) {
      label.value = item.textContent || '';
    } else {
      label.textContent = item.textContent;
    }

    switch (messageAction) {
      case 'changeTheme':
        setThemeMode(item.dataset.value);
        break;
      case 'changeSettingsTheme':
        setSurfaceTheme('settings', item.dataset.value);
        break;
      case 'changeCandTheme':
        setSurfaceTheme('cand', item.dataset.value);
        break;
      case 'changeFtbTheme':
        setSurfaceTheme('ftb', item.dataset.value);
        break;
      case 'changeMenuTheme':
        setSurfaceTheme('menu', item.dataset.value);
        break;
      case 'changeEmojiTheme':
        setSurfaceTheme('emoji', item.dataset.value);
        break;
      case 'changeScreenKeyboardTheme':
        setSurfaceTheme('screenKeyboard', item.dataset.value);
        break;
      case 'changeHandwritingTheme':
        setSurfaceTheme('handwriting', item.dataset.value);
        break;
      case 'changeVoiceTheme':
        setSurfaceTheme('voice', item.dataset.value);
        break;
      case 'changeCandidateArrange':
        applyCandidateArrange(item.dataset.value);
        break;
      default:
        break;
    }

    // Always run transform first so local preview updates even without WebView2
    // (e.g. vite browser preview).
    const nextValue = configPath
      ? valueTransform(item.dataset.value ?? '')
      : (item.dataset.value ?? '');

    if (window.chrome?.webview && configPath) {
      window.chrome.webview.postMessage(serializeHostMessage({
        type: 'configUpdate',
        data: {
          path: configPath,
          value: nextValue
        }
      }));
    } else if (window.chrome?.webview && messageAction === 'changeCandidateArrange') {
      window.chrome.webview.postMessage(serializeHostMessage({
        type: 'configUpdate',
        data: {
          path: 'appearance.candidate_window_layout',
          value: item.dataset.value ?? ''
        }
      }));

    }

    menu.classList.remove('open');
  }, { signal });

  // 点击外部关闭
  document.addEventListener('click', (e: Event) => {
    const target = e.target as Node;
    if (!btn.contains(target) && !menu.contains(target)) {
      menu.classList.remove('open');
    }
  }, { signal });
}

export function applyDropdownValue(btnId: string, menuId: string, value: string | undefined): void {
  if (!value) {
    return;
  }

  const btnLabel = document.querySelector<HTMLElement>(`#${btnId} span, #${btnId} input`);
  if (!btnLabel) {
    return;
  }

  const item = Array.from(document.querySelectorAll<HTMLElement>(`#${menuId} .dropdown-item`)).find(
    (el) => el.dataset.value === value
  );
  const label = item?.textContent || value;
  if (btnLabel instanceof HTMLInputElement) {
    btnLabel.value = label;
  } else {
    btnLabel.textContent = label;
  }
}

// Rows styled up front when the menu is built; the rest wait until scrolled into view.
const FONT_PREVIEW_EAGER_ROWS = 12;

const fontPreviewObservers = new WeakMap<HTMLElement, IntersectionObserver>();

function applyPreviewFont(item: HTMLElement): void {
  const family = item.dataset.previewFamily;
  if (!family) {
    return;
  }
  item.style.fontFamily = family;
  delete item.dataset.previewFamily;
}

/**
 * Resolving a font family costs a font lookup per row, so a few hundred rows
 * styled at once freeze the window for seconds when the menu first paints.
 */
function observeFontPreview(menu: HTMLElement, items: HTMLElement[]): void {
  fontPreviewObservers.get(menu)?.disconnect();

  const observer = new IntersectionObserver(
    (entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) {
          continue;
        }
        applyPreviewFont(entry.target as HTMLElement);
        observer.unobserve(entry.target);
      }
    },
    { root: menu, rootMargin: '120px 0px' }
  );

  fontPreviewObservers.set(menu, observer);
  items.forEach((item) => observer.observe(item));
}

export function populateDropdownMenu(
  menuId: string,
  values: string[],
  options?: { selected?: string; previewFont?: boolean; previewFamily?: (name: string) => string }
): void {
  const menu = document.getElementById(menuId);
  if (!menu) {
    return;
  }

  const names = [...values];
  const selected = options?.selected;
  if (selected && !names.includes(selected)) {
    names.unshift(selected);
  }

  const fragment = document.createDocumentFragment();
  const deferredPreview: HTMLElement[] = [];
  names.forEach((name, index) => {
    const item = document.createElement('div');
    item.className = 'dropdown-item';
    item.dataset.value = name;
    item.textContent = name;
    if (options?.previewFont) {
      const previewName = options.previewFamily?.(name) || name;
      // Escape the escape character first, as in appearance.ts quoteFont: quoting only `"` leaves a
      // name ending in a backslash able to consume the closing quote and turn the rest of the
      // fontFamily string into CSS.
      const quoted = /\s/.test(previewName)
        ? `"${previewName.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`
        : previewName;
      const family = `${quoted}, sans-serif`;
      if (index < FONT_PREVIEW_EAGER_ROWS) {
        item.style.fontFamily = family;
      } else {
        item.dataset.previewFamily = family;
        deferredPreview.push(item);
      }
    }
    fragment.appendChild(item);
  });
  menu.replaceChildren(fragment);

  if (deferredPreview.length > 0) {
    observeFontPreview(menu, deferredPreview);
  }
}

// 切换按钮功能
export function setupToggleButton(btnId: string, onChanged?: (active: boolean) => void): void {
  const toggle = document.getElementById(btnId);
  if (!toggle) {
    console.warn(`Toggle button not found: ${btnId}`);
    return;
  }

  const toggleState = () => {
    // 置灰开关（如智能标点子开关在总开关关闭时）保留展示状态，但不接受点击或键盘激活。
    if (toggle.getAttribute('aria-disabled') === 'true') return;
    toggle.classList.toggle('active');
    const active = toggle.classList.contains('active');
    toggle.setAttribute('aria-checked', String(active));
    onChanged?.(active);
  };

  toggle.addEventListener('click', toggleState);
  toggle.addEventListener('keydown', (event: KeyboardEvent) => {
    if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      toggleState();
    }
  });
}

export function applyToggleState(btnId: string, active: boolean): void {
  const toggle = document.getElementById(btnId);
  toggle?.classList.toggle('active', active);
  toggle?.setAttribute('aria-checked', String(active));
}

// 模糊音规则复选随总开关联动：禁用 + 置灰 + 提示。input.ts（点击总开关）与
// config-sync.ts（快照回填）共用这一份 DOM 名单，不要在两侧各写一遍。
export function setFuzzyRuleOptionsDisabled(disabled: boolean): void {
  document.querySelectorAll<HTMLInputElement>('input[name="fuzzy-rule"]').forEach((checkbox) => {
    checkbox.disabled = disabled;
  });
  document.getElementById('fuzzyDetails')?.classList.toggle('is-disabled', disabled);
  document.getElementById('fuzzyDisabledHint')?.classList.toggle('is-hidden', !disabled);
}

// 智能标点子开关随总开关联动：置灰 + 禁用 + 提示，勾选状态保留展示。
// input.ts（点击总开关）与 config-sync.ts（快照回填）共用这一份 DOM 名单。
const SMART_PUNCTUATION_OPTION_IDS = [
  'smartPunctuationSpaceConvertToggleBtn',
  'smartPunctuationRepeatToChineseToggleBtn',
  'smartPunctuationDirectDigitToggleBtn',
  'smartPunctuationDirectLetterToggleBtn'
];

export function setSmartPunctuationOptionsDisabled(disabled: boolean): void {
  for (const id of SMART_PUNCTUATION_OPTION_IDS) {
    const toggle = document.getElementById(id);
    toggle?.setAttribute('aria-disabled', String(disabled));
    if (toggle) toggle.tabIndex = disabled ? -1 : 0;
  }
  document.getElementById('smartPunctuationDetails')?.classList.toggle('is-disabled', disabled);
  document.getElementById('smartPunctuationDisabledHint')?.classList.toggle('is-hidden', !disabled);
}

// 候选混输子开关随总开关联动：置灰 + 禁用 + 提示，勾选状态保留展示。
// tools-settings.ts（点击总开关）与 config-sync.ts（快照回填）共用这一份 DOM 名单。
const MIXED_CANDIDATE_OPTION_IDS = [
  'zhEnToggleBtn',
  'emojiMixedInputToggleBtn',
  'kaomojiMixedInputToggleBtn',
  'quickPhraseCandidatesToggleBtn',
  'quickPhraseFrequencyToggleBtn',
  'dateTimeCandidatesToggleBtn',
  'dateTimeMenuToggleBtn'
];

export function setMixedCandidateOptionsDisabled(disabled: boolean): void {
  for (const id of MIXED_CANDIDATE_OPTION_IDS) {
    const toggle = document.getElementById(id);
    toggle?.setAttribute('aria-disabled', String(disabled));
    if (toggle) toggle.tabIndex = disabled ? -1 : 0;
  }
  document.getElementById('mixedCandidatesDetails')?.classList.toggle('is-disabled', disabled);
  document.getElementById('mixedCandidatesDisabledHint')?.classList.toggle('is-hidden', !disabled);
}

export function applyCandidateArrange(value: string | undefined): void {
  if (value !== 'horizontal' && value !== 'vertical') {
    return;
  }

  const wnd_h = document.getElementById('candidate-wnd-h');
  const wnd_v = document.getElementById('candidate-wnd-v');
  const label = document.querySelector<HTMLElement>('#arrangeBtn span');

  if (wnd_h) wnd_h.style.display = value === 'horizontal' ? 'flex' : 'none';
  if (wnd_v) wnd_v.style.display = value === 'vertical' ? 'flex' : 'none';
  if (label) label.textContent = value === 'horizontal' ? '横向' : '纵向';
}
