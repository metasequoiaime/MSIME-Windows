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
