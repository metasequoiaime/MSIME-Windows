import { loadHTML, showOnlyCurrentModule } from '../utils/common-utils';
import { notifySettingsModuleReady } from './config-sync';

const BACKGROUND_MODULES = [
  'input',
  'helpcode',
  'shortcut',
  'dict',
  'voice',
  'screenkb-settings',
  'handwriting-settings',
  'tools-settings',
  'ai-settings',
  'floating-toolbar',
  'help-settings',
  'about-settings',
  'feedback-settings'
] as const;

const setupLoaders: Record<string, () => Promise<void>> = {
  appearance: async () => {
    const module = await import('./appearance');
    await module.setupAppearance();
  },
  statistics: async () => {
    (await import('./statistics')).setupStatistics();
  },
  input: async () => {
    (await import('./input')).setupInput();
  },
  helpcode: async () => {
    (await import('./helpcode')).setupHelpcode();
  },
  dict: async () => {
    (await import('./dict')).setupDictionary();
  },
  skin: async () => {
    await (await import('./skin')).setupSkin();
  },
  voice: async () => {
    (await import('./voice')).setupVoiceInput();
  },
  'screenkb-settings': async () => {
    (await import('./screenkb-settings')).setupScreenKeyboardSettings();
  },
  'handwriting-settings': async () => {
    (await import('./handwriting-settings')).setupHandwritingSettings();
  },
  'tools-settings': async () => {
    (await import('./tools-settings')).setupToolsSettings();
  },
  'ai-settings': async () => {
    (await import('./ai-settings')).setupAiSettings();
  },
  shortcut: async () => {
    (await import('./shortcut')).setupShortcut();
  },
  'floating-toolbar': async () => {
    (await import('./floating-toolbar')).setupFloatingToolbar();
  },
  'about-settings': async () => {
    (await import('./about-settings')).setupAboutSettings();
  },
  'feedback-settings': async () => {
    (await import('./feedback-settings')).setupFeedbackSettings();
  }
};

const loadedModules = new Set<string>();
const inflightModules = new Map<string, Promise<void>>();

export async function loadContent(moduleName: string): Promise<void> {
  if (loadedModules.has(moduleName)) return;
  const inflight = inflightModules.get(moduleName);
  if (inflight) {
    await inflight;
    return;
  }

  const task = (async () => {
    try {
      const container = document.getElementById(moduleName);
      if (!container) return;
      if (!container.innerHTML.trim()) {
        container.innerHTML = await loadHTML(`/src/partials/${moduleName}.html`);
      }
      await setupLoaders[moduleName]?.();
      loadedModules.add(moduleName);
      notifySettingsModuleReady(moduleName);
    } finally {
      inflightModules.delete(moduleName);
    }
  })();

  inflightModules.set(moduleName, task);
  await task;
}

export function scheduleBackgroundModuleLoad(): void {
  const run = async () => {
    for (const name of BACKGROUND_MODULES) {
      try {
        await loadContent(name);
      } catch {
        // Keep remaining sections loading even if one partial fails.
      }
      await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
    }
  };

  window.setTimeout(() => {
    void run();
  }, 1200);
}

export function setupSidebar(): void {
  const moveIndicator = setupSidebarIndicator();
  const sidebarItems = document.querySelectorAll('.sidebar .item');
  const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  sidebarItems.forEach((item: Element) => {
    item.addEventListener('click', () => {
      if (item.classList.contains('active')) {
        return;
      }
      const currentActive = document.querySelector('.sidebar .item.active');
      currentActive?.classList.remove('active');
      item.classList.add('active');

      const htmlItem = item as HTMLElement;
      // 先把指示条交给合成层动画，再做右侧换页，避免重排打断滑动。
      moveIndicator(htmlItem, { animate: !reduceMotion });

      const targetId = htmlItem.dataset.target;
      if (targetId) {
        void loadContent(targetId).then(() => showOnlyCurrentModule(targetId));
      }
    });
  });
}

type MoveIndicatorOptions = {
  animate?: boolean;
};

function setupSidebarIndicator(): (item: HTMLElement, options?: MoveIndicatorOptions) => void {
  const sidebar = document.querySelector('.sidebar') as HTMLElement | null;
  if (!sidebar) {
    return () => { };
  }

  const currentActive = sidebar.querySelector('.item.active') as HTMLElement | null;
  const indicator = document.createElement('div');
  indicator.className = 'active-indicator';

  let currentX = 0;
  let currentY = 0;
  let hasPositioned = false;
  let activeAnimation: Animation | null = null;

  const readTarget = (item: HTMLElement) => {
    const barHeight = 22;
    return {
      x: item.offsetLeft - 2,
      y: item.offsetTop + (item.offsetHeight - barHeight) / 2,
    };
  };

  const commitTransform = (x: number, y: number) => {
    currentX = x;
    currentY = y;
    indicator.style.transform = `translate3d(${x}px, ${y}px, 0)`;
  };

  const cancelActiveAnimation = () => {
    if (!activeAnimation) {
      return;
    }
    try {
      activeAnimation.commitStyles();
    } catch {
      // ignore: element may already match the target style
    }
    activeAnimation.cancel();
    activeAnimation = null;

    const matrix = new DOMMatrix(getComputedStyle(indicator).transform);
    currentX = matrix.m41;
    currentY = matrix.m42;
  };

  const durationForDistance = (dx: number, dy: number) => {
    const distance = Math.hypot(dx, dy);
    // 相邻项距离短，用更短时长，避免强 ease-out 在末段「粘滞」。
    return Math.round(Math.min(280, Math.max(140, 110 + distance * 0.9)));
  };

  if (currentActive) {
    const { x, y } = readTarget(currentActive);
    commitTransform(x, y);
    indicator.style.opacity = '1';
    indicator.style.visibility = 'visible';
    hasPositioned = true;
  } else {
    indicator.style.visibility = 'hidden';
  }

  sidebar.appendChild(indicator);

  const moveIndicator = (item: HTMLElement, options: MoveIndicatorOptions = {}) => {
    const { x, y } = readTarget(item);
    const shouldAnimate = options.animate !== false;

    if (!hasPositioned) {
      hasPositioned = true;
      commitTransform(x, y);
      indicator.style.opacity = '1';
      indicator.style.visibility = 'visible';
      return;
    }

    if (!shouldAnimate) {
      cancelActiveAnimation();
      commitTransform(x, y);
      indicator.style.opacity = '1';
      return;
    }

    cancelActiveAnimation();
    const duration = durationForDistance(x - currentX, y - currentY);
    const targetTransform = `translate3d(${x}px, ${y}px, 0)`;

    activeAnimation = indicator.animate(
      [
        { transform: `translate3d(${currentX}px, ${currentY}px, 0)` },
        { transform: targetTransform },
      ],
      {
        duration,
        easing: 'cubic-bezier(0.22, 1, 0.36, 1)',
        fill: 'forwards',
      },
    );

    const animation = activeAnimation;
    animation.finished.then(() => {
      if (activeAnimation !== animation) {
        return;
      }
      try {
        animation.commitStyles();
      } catch {
        indicator.style.transform = targetTransform;
      }
      animation.cancel();
      activeAnimation = null;
      currentX = x;
      currentY = y;
    }).catch(() => {
      // cancelled
    });

    indicator.style.opacity = '1';
  };

  window.addEventListener('resize', () => {
    const activeItem = sidebar.querySelector('.item.active') as HTMLElement | null;
    if (activeItem) {
      moveIndicator(activeItem, { animate: false });
    }
  });

  return moveIndicator;
}
