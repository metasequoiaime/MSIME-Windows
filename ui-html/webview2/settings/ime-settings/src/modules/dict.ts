import { createDictionaryPager, DICTIONARY_PAGE_SIZE } from '../utils/dictionary-pagination';
import { readDictionaryFile } from '../utils/dictionary-file';
import { onHostMessage } from '../utils/host-messages';
import type { SettingsMessage } from '../../../../shared/messages';
type DictionaryRequest = Extract<SettingsMessage, { type: 'dictionaryRequest' }>['data'];
import { serializeHostMessage } from '../../../../shared/messages';
type DictionaryType = 'quanpin' | 'wubi' | 'english';
type DictionaryRow = { code?: string; word: string; weight?: number; display?: string };

let dictionary: DictionaryType = 'quanpin';
let editing: DictionaryRow | null = null;
let requestCounter = 0;
let lastQuery = '';
const pendingRequests = new Map<string, { action: DictionaryRequest['action']; dictionary: string }>();
let latestQueryId = '';
let pager: ReturnType<typeof createDictionaryPager> | undefined;
let toastTimer: number | null = null;

function post(action: DictionaryRequest['action'], data: Partial<Omit<DictionaryRequest, 'action' | 'requestId'>> = {}): void {
  const requestId = `dict-${++requestCounter}`;
  const targetDictionary = dictionary;
  pendingRequests.set(requestId, { action, dictionary: targetDictionary });
  if (action === 'query') { latestQueryId = requestId; pager?.loading(); }
  window.chrome?.webview?.postMessage(serializeHostMessage({
    type: 'dictionaryRequest',
    data: { requestId, dictionary, action, ...data }
  }));
}

function showToast(message: string, ok: boolean, durationMs = 3200): void {
  const toast = document.getElementById('dictToast');
  if (!toast) return;
  const table = document.querySelector<HTMLElement>('.dict-table-wrap');
  if (table) {
    const rect = table.getBoundingClientRect();
    toast.style.left = `${rect.left + rect.width / 2}px`;
  }
  const messageElement = document.getElementById('dictToastMessage');
  const iconElement = document.getElementById('dictToastIcon');
  if (messageElement) messageElement.textContent = message;
  if (iconElement) iconElement.textContent = ok ? '' : '!';
  toast.className = `dict-toast visible ${ok ? 'success' : 'error'}`;
  if (toastTimer !== null) window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => {
    toast.classList.remove('visible');
    toastTimer = null;
  }, durationMs);
}

function renderRows(rows: DictionaryRow[]): void {
  const body = document.getElementById('dictRows');
  if (!body) return;
  if (!rows.length) {
    body.innerHTML = '<tr><td colspan="5" class="dict-empty">没有找到词条</td></tr>';
    syncTableHeaderWidth();
    return;
  }
  body.replaceChildren(...rows.map((row, index) => {
    const tr = document.createElement('tr');
    const indexCell = document.createElement('td');
    indexCell.className = 'dict-index-column';
    indexCell.textContent = String((pager?.offset ?? 0) + index + 1);
    tr.appendChild(indexCell);
    const values = dictionary === 'english'
      ? [row.word, row.display ?? row.word, String(row.weight ?? 0)]
      : [row.code ?? '', row.word, String(row.weight ?? 0)];
    values.forEach((value) => {
      const td = document.createElement('td');
      td.textContent = value;
      td.addEventListener('mouseenter', () => {
        if (td.scrollWidth > td.clientWidth) td.title = value;
        else td.removeAttribute('title');
      });
      tr.appendChild(td);
    });
    const actions = document.createElement('td');
    const edit = document.createElement('button'); edit.className = 'dict-row-action'; edit.textContent = '编辑';
    edit.addEventListener('click', () => openDialog(row));
    const remove = document.createElement('button'); remove.className = 'dict-row-action danger'; remove.textContent = '删除';
    remove.addEventListener('click', () => {
      if (!window.confirm(`确定删除“${row.word}”吗？`)) return;
      if (dictionary === 'english') post('delete', { oldWord: row.word, oldDisplay: row.display ?? row.word, word: row.word, display: row.display ?? row.word });
      else post('delete', { oldCode: row.code, oldWord: row.word, code: row.code, word: row.word, weight: row.weight });
    });
    actions.append(edit, remove); tr.appendChild(actions); return tr;
  }));
  syncTableHeaderWidth();
}

function syncTableHeaderWidth(): void {
  window.requestAnimationFrame(() => {
    const scrollArea = document.querySelector<HTMLElement>('.dict-table-wrap');
    const header = document.getElementById('dictTableHeaderWrap');
    if (!scrollArea || !header) return;
    header.style.paddingRight = `${scrollArea.offsetWidth - scrollArea.clientWidth}px`;
  });
}

function updateMode(): void {
  latestQueryId = '';
  pager?.reset();
  const english = dictionary === 'english';
  const quanpin = dictionary === 'quanpin';
  const search = document.getElementById('dictSearch') as HTMLInputElement;
  search.value = '';
  search.placeholder = english ? '输入英文前缀，例如 meta' : quanpin
    ? '输入完整全拼，例如 nihao' : '输入五笔编码前缀';
  document.getElementById('dictHint')!.textContent = english
    ? '按英文前缀查询；批量导入格式为：单词<Tab>显示内容<Tab>权重（兼容无权重的两列文件）'
    : quanpin
      ? '全拼新增会校验拼音合法性、汉字数量和重复词条；批量导入格式为：词语<Tab>全拼<Tab>权重（仅支持 Tab 分隔，全拼可用 \' 分音节，如 ni\'hao）'
      : '管理 86 五笔编码、词条及权重；批量导入格式为：词语<Tab>五笔编码<Tab>权重（仅支持 Tab 分隔）';
  const importButton = document.getElementById('dictImportButton') as HTMLButtonElement | null;
  if (importButton) importButton.style.display = '';
  document.getElementById('dictTableHeader')!.innerHTML = english
    ? '<th class="dict-index-column">No.</th><th>单词</th><th>显示内容</th><th>权重</th><th>操作</th>'
    : '<th class="dict-index-column">No.</th><th>编码</th><th>词条</th><th>权重</th><th>操作</th>';
  document.getElementById('dictRows')!.innerHTML = '<tr><td colspan="5" class="dict-empty">输入查询条件后查看词条</td></tr>';
  syncTableHeaderWidth();
}

function openDialog(row: DictionaryRow | null = null): void {
  editing = row;
  const english = dictionary === 'english';
  document.getElementById('dictDialogTitle')!.textContent = row ? '编辑词条' : '新增词条';
  document.getElementById('dictCodeField')!.firstChild!.textContent = english ? '单词' : dictionary === 'quanpin' ? '全拼' : '五笔编码';
  document.getElementById('dictWordField')!.firstChild!.textContent = english ? '显示内容' : '词条';
  (document.getElementById('dictCode') as HTMLInputElement).value = row
    ? (english ? row.word : row.code ?? '')
    : '';
  document.getElementById('dictWeightField')!.style.display = 'grid';
  (document.getElementById('dictWord') as HTMLInputElement).value = english ? row?.display ?? '' : row?.word ?? '';
  (document.getElementById('dictWeight') as HTMLInputElement).value = row?.weight === undefined ? '10' : String(row.weight);
  const modal = document.getElementById('dictModal')!; modal.classList.add('open'); modal.setAttribute('aria-hidden', 'false');
}

function closeDialog(): void {
  const modal = document.getElementById('dictModal')!; modal.classList.remove('open'); modal.setAttribute('aria-hidden', 'true'); editing = null;
}

function query(offset = 0): void {
  if (offset === 0) lastQuery = (document.getElementById('dictSearch') as HTMLInputElement).value.trim();
  if (!lastQuery) { showToast('请输入查询内容', false); return; }
  post('query', { ...(dictionary === 'english' ? { word: lastQuery } : { code: lastQuery }), offset, limit: DICTIONARY_PAGE_SIZE });
}

function downloadExport(content: string, filename: string): void {
  const blob = new Blob([new Uint8Array([0xef, 0xbb, 0xbf]), content], {
    type: 'text/plain;charset=utf-8',
  });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = filename;
  anchor.style.display = 'none';
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
}

export function setupDictionary(): void {
  const table = document.querySelector<HTMLElement>('.dict-table-wrap');
  if (table) pager = createDictionaryPager(table, offset => query(offset));
  document.querySelectorAll<HTMLButtonElement>('.dict-tab').forEach((tab) => tab.addEventListener('click', () => {
    document.querySelector('.dict-tab.active')?.classList.remove('active'); tab.classList.add('active');
    dictionary = tab.dataset.dictionary as DictionaryType; updateMode();
  }));
  document.getElementById('dictSearchButton')?.addEventListener('click', () => query());
  document.getElementById('dictSearch')?.addEventListener('keydown', (event) => { if ((event as KeyboardEvent).key === 'Enter') query(); });
  document.getElementById('dictAddButton')?.addEventListener('click', () => openDialog());
  document.getElementById('dictImportButton')?.addEventListener('click', () => {
    (document.getElementById('dictImportFile') as HTMLInputElement | null)?.click();
  });
  document.getElementById('dictImportFile')?.addEventListener('change', async (event) => {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = '';
    if (!file) return;
    try {
      const content = await readDictionaryFile(file);
      if (!content.trim()) { showToast('文件内容为空', false); return; }
      post('import', { content });
    } catch {
      showToast('读取文件失败', false);
    }
  });
  document.getElementById('dictImportHansButton')?.addEventListener('click', () => {
    (document.getElementById('dictImportHansFile') as HTMLInputElement | null)?.click();
  });
  document.getElementById('dictImportHansFile')?.addEventListener('change', async (event) => {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = '';
    if (!file) return;
    try {
      const content = await readDictionaryFile(file);
      if (!content.trim()) { showToast('文件内容为空', false); return; }
      post('importHans', { content });
    } catch {
      showToast('读取文件失败', false);
    }
  });
  document.querySelectorAll<HTMLButtonElement>('[data-export-dictionary]').forEach((button) => {
    button.addEventListener('click', () => {
      const exportDictionary = button.dataset.exportDictionary as DictionaryType | undefined;
      if (!exportDictionary) return;
      post('export', { dictionary: exportDictionary });
    });
  });
  document.getElementById('dictCancelButton')?.addEventListener('click', closeDialog);
  document.getElementById('dictToastClose')?.addEventListener('click', () => {
    document.getElementById('dictToast')?.classList.remove('visible');
    if (toastTimer !== null) {
      window.clearTimeout(toastTimer);
      toastTimer = null;
    }
  });
  window.addEventListener('resize', syncTableHeaderWidth);
  document.addEventListener('keydown', (event: KeyboardEvent) => {
    if (event.key === 'Escape' && document.getElementById('dictModal')?.classList.contains('open')) {
      event.preventDefault();
      closeDialog();
      (document.activeElement as HTMLElement | null)?.blur();
    }
  });
  document.getElementById('dictSaveButton')?.addEventListener('click', () => {
    const code = (document.getElementById('dictCode') as HTMLInputElement).value.trim();
    const word = (document.getElementById('dictWord') as HTMLInputElement).value.trim();
    const weight = Number((document.getElementById('dictWeight') as HTMLInputElement).value);
    if (dictionary === 'english') post(editing ? 'update' : 'create', {
      word: code, display: word, weight, oldWord: editing?.word, oldDisplay: editing?.display ?? editing?.word,
    });
    else post(editing ? 'update' : 'create', { code, word, weight, oldCode: editing?.code, oldWord: editing?.word });
  });
  onHostMessage('dictionaryResponse', payload => {
    if (!payload.requestId.startsWith('dict-')) return;
    const context = pendingRequests.get(payload.requestId);
    if (!context) return;
    pendingRequests.delete(payload.requestId);
    if (context.action === 'query' && payload.requestId !== latestQueryId) return;
    if (context.action === 'query' && context.dictionary !== dictionary) return;
    const lastAction = context.action;
    if (lastAction === 'query') {
      if (payload.ok) pager?.update(payload.offset ?? 0, payload.rows.length, payload.hasMore === true);
      else pager?.failed();
    }

    const isImport = lastAction === 'import' || lastAction === 'importHans';
    const isExport = lastAction === 'export';
    if (isExport && payload.ok && typeof payload.content === 'string' && typeof payload.filename === 'string') {
      downloadExport(payload.content, payload.filename);
    }
    showToast(
      payload.message ?? (payload.ok ? '操作成功' : '操作失败'),
      Boolean(payload.ok),
      isImport ? 5600 : 3200,
    );
    if (lastAction === 'query' && payload.ok) renderRows(payload.rows);
    if (payload.ok && lastAction !== 'query' && !isExport && context.dictionary === dictionary) { closeDialog(); if (lastQuery) query(pager?.offset ?? 0); }
  });
  updateMode();
}
