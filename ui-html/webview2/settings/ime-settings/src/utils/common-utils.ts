// HTML 加载工具
// Prefer bundling partials so they work in WebView2/file://
const partials = import.meta.glob<string>('/src/partials/**/*.html', {
  query: '?raw',
  import: 'default',
});

export async function loadHTML(url: string): Promise<string> {
  if (url.startsWith('/src/partials/')) {
    const loader = partials[url];
    if (!loader) {
      throw new Error(`Partial not found: ${url}`);
    }
    return await loader();
  }

  const response = await fetch(url);
  return await response.text();
}

const MODULE_IDS = [
  'floating-toolbar',
  'appearance',
  'statistics',
  'input',
  'helpcode',
  'dict',
  'skin',
  'voice',
  'screenkb-settings',
  'handwriting-settings',
  'tools-settings',
  'ai-settings',
  'shortcut',
  'help-settings',
  'about-settings',
  'feedback-settings',
] as const;

let activeModuleName: string | null = null;

// 只显示当前模块。首次调用会一次性隐藏其余模块；之后只切换上一页 / 下一页。
export function showOnlyCurrentModule(moduleName: string): void {
  if (activeModuleName === moduleName) {
    return;
  }

  const next = document.getElementById(moduleName);
  if (!next) {
    return;
  }

  if (activeModuleName === null) {
    for (const id of MODULE_IDS) {
      if (id === moduleName) {
        continue;
      }
      const el = document.getElementById(id);
      if (el) {
        el.style.display = 'none';
      }
    }
  } else {
    const prev = document.getElementById(activeModuleName);
    if (prev) {
      prev.style.display = 'none';
    }
  }

  next.style.display = 'block';
  activeModuleName = moduleName;
  // 模块只在首次打开时装配，重新切回不会重跑 setup；需要按「再次可见」刷新的模块
  // （如统计面板）监听这个事件。
  window.dispatchEvent(new CustomEvent('msime:module-shown', { detail: { module: moduleName } }));
}
