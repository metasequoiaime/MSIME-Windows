import { serializeHostMessage } from '../../../../shared/messages';
import { applyDropdownValue, applyToggleState, setFuzzyRuleOptionsDisabled, setupDropdownMenu, setupToggleButton } from './shared';
import { updateConfig } from './config-sync';
import { updateCandidatePreviewHelpcode } from './appearance';

type InputScheme = 'quanpin' | 'shuangpin' | 'wubi';
type InputMode = 'chinese' | 'japanese';

let applyingInputConfig = false;
let usingCustomTranslation = false;

function updateInputConfig(path: string, value: string): void {
  window.chrome?.webview?.postMessage(serializeHostMessage({
    type: 'configUpdate',
    data: { path, value }
  }));
}

export function applyInputConfig(
  inputMode: string | undefined,
  schema: string | undefined,
  characterSet: string | undefined,
  shuangpinSchema: string | undefined,
  wubiSchema: string | undefined,
  defaultImeMode?: string | undefined,
  imeModeScope?: string | undefined,
  japaneseSchema?: string | undefined
): void {
  applyingInputConfig = true;
  try {
    const mode: InputMode = inputMode === 'japanese' ? 'japanese' : 'chinese';
  const modeRadio = document.querySelector<HTMLInputElement>(`input[name="input-mode"][value="${mode}"]`);
  if (modeRadio) modeRadio.checked = true;
  syncInputModeView(mode);

  if (schema === 'quanpin' || schema === 'shuangpin' || schema === 'wubi') {
    const radio = document.querySelector<HTMLInputElement>(`input[name="input-method"][value="${schema}"]`);
    if (radio) {
      radio.checked = true;
    }
    updateCandidatePreviewHelpcode({ input_schema: schema });
  }

  applyDropdownValue('characterSetBtn', 'characterSetMenu', characterSet);
  applyDropdownValue('shuangpinSchemeBtn', 'shuangpinSchemeMenu', shuangpinSchema);
  applyDropdownValue('wubiSchemeBtn', 'wubiSchemeMenu', wubiSchema);
  applyDropdownValue('defaultImeModeBtn', 'defaultImeModeMenu', defaultImeMode);
  applyDropdownValue('imeModeScopeBtn', 'imeModeScopeMenu', imeModeScope);
  const japaneseRadio = document.querySelector<HTMLInputElement>(
    `input[name="japanese-input-method"][value="${japaneseSchema === 'romaji' ? japaneseSchema : 'romaji'}"]`
  );
  if (japaneseRadio) japaneseRadio.checked = true;
  } finally {
    applyingInputConfig = false;
  }
}

function syncInputModeView(mode: InputMode): void {
  const japanese = mode === 'japanese';
  document.querySelectorAll<HTMLElement>('.chinese-scheme-settings').forEach((element) => {
    element.hidden = japanese;
  });
  document.querySelectorAll<HTMLElement>('.japanese-scheme-settings').forEach((element) => {
    element.hidden = !japanese;
  });
}

export function applyFrequencyConfig(config: any): void {
  applyDropdownValue('frequencyModeBtn', 'frequencyModeMenu', config?.mode);
  applyDropdownValue('frequencyTriggerCountBtn', 'frequencyTriggerCountMenu', String(config?.trigger_count ?? 1));
  applyDropdownValue('frequencyLinearStepBtn', 'frequencyLinearStepMenu', String(config?.linear_step ?? 1));
}

function syncZhEnMixedInputOptionsEnabled(enabled: boolean): void {
  document.getElementById('zhEnMixedInputOptions')?.classList.toggle('is-disabled', !enabled);
}

export function applyZhEnMixedInputConfig(enabled?: boolean, minChars?: number): void {
  if (typeof enabled === 'boolean') {
    applyToggleState('zhEnToggleBtn', enabled);
    syncZhEnMixedInputOptionsEnabled(enabled);
  }
  if (typeof minChars === 'number' && Number.isFinite(minChars)) {
    applyDropdownValue('zhEnTriggerLengthBtn', 'zhEnTriggerLengthMenu', String(minChars));
  }
}

function syncCandidateTranslationOptions(enabled: boolean): void {
  document.getElementById('candidateTranslationApiOptions')?.classList.toggle('is-disabled', !enabled);
}

function syncCandidateTranslationWarning(): void {
  const warning = document.getElementById('candidateTranslationApiWarning');
  if (!warning) return;
  if (usingCustomTranslation) {
    const endpoint = (document.getElementById('customTranslationEndpoint') as HTMLInputElement | null)?.value.trim();
    const valid = /^https?:\/\/\S+$/i.test(endpoint ?? '');
    warning.textContent = '请填写以 http:// 或 https:// 开头的完整接口地址';
    warning.classList.toggle('is-hidden', valid);
    return;
  }
  const secretId = (document.getElementById('tencentTmtSecretId') as HTMLInputElement | null)?.value.trim();
  const secretKey = (document.getElementById('tencentTmtSecretKey') as HTMLInputElement | null)?.value.trim();
  warning.textContent = '请填写 SecretId 和 SecretKey 后使用云端翻译';
  warning.classList.toggle('is-hidden', Boolean(secretId && secretKey));
}

function syncTranslationProviderView(useCustom: boolean): void {
  usingCustomTranslation = useCustom;
  const tencentFields = document.getElementById('tencentTranslationFields');
  const customFields = document.getElementById('customTranslationFields');
  if (tencentFields) tencentFields.hidden = useCustom;
  if (customFields) customFields.hidden = !useCustom;
  syncCandidateTranslationWarning();
}

export function applyTencentTmtConfig(config: Record<string, unknown> | undefined): void {
  const secretId = document.getElementById('tencentTmtSecretId') as HTMLInputElement | null;
  const secretKey = document.getElementById('tencentTmtSecretKey') as HTMLInputElement | null;
  if (secretId && typeof config?.secret_id === 'string') secretId.value = config.secret_id;
  if (secretKey && typeof config?.secret_key === 'string') secretKey.value = config.secret_key;
  applyDropdownValue(
    'translationTargetLanguageBtn',
    'translationTargetLanguageMenu',
    typeof config?.target_language === 'string' ? config.target_language : 'en'
  );
  syncCandidateTranslationWarning();
}

export function applyCustomTranslationConfig(config: Record<string, unknown> | undefined): void {
  const useCustom = config?.enabled === true;
  applyDropdownValue('translationProviderBtn', 'translationProviderMenu', useCustom ? 'custom' : 'tencent');
  const endpoint = document.getElementById('customTranslationEndpoint') as HTMLInputElement | null;
  const apiKey = document.getElementById('customTranslationApiKey') as HTMLInputElement | null;
  if (endpoint && typeof config?.endpoint === 'string') endpoint.value = config.endpoint;
  if (apiKey && typeof config?.api_key === 'string') apiKey.value = config.api_key;
  syncTranslationProviderView(useCustom);
}

function setupSecretVisibility(inputId: string, buttonId: string, name: string): void {
  const input = document.getElementById(inputId) as HTMLInputElement | null;
  const button = document.getElementById(buttonId) as HTMLButtonElement | null;
  button?.addEventListener('click', () => {
    if (!input) return;
    const show = input.type === 'password';
    input.type = show ? 'text' : 'password';
    button.setAttribute('aria-pressed', String(show));
    const label = `${show ? '隐藏' : '显示'} ${name}`;
    button.setAttribute('aria-label', label);
    button.title = label;
  });
}

export function setupInput(): void {
  document.querySelectorAll<HTMLInputElement>('input[name="input-mode"]').forEach((radio) => {
    radio.addEventListener('change', () => {
      if (applyingInputConfig) return;
      if (!radio.checked || (radio.value !== 'chinese' && radio.value !== 'japanese')) return;
      const mode = radio.value as InputMode;
      syncInputModeView(mode);
      updateInputConfig('input.mode', mode);
    });
  });

  document.querySelectorAll<HTMLInputElement>('input[name="japanese-input-method"]').forEach((radio) => {
    radio.addEventListener('change', () => {
      if (radio.checked && radio.value === 'romaji' && !applyingInputConfig) {
        updateInputConfig('input.japanese_schema', radio.value);
      }
    });
  });

  setupDropdownMenu('characterSetBtn', 'characterSetMenu', 'changeCharacterSet', true, 'input.character_set');
  setupDropdownMenu('defaultImeModeBtn', 'defaultImeModeMenu', '', true, 'input.default_ime_mode');
  setupDropdownMenu('imeModeScopeBtn', 'imeModeScopeMenu', '', true, 'input.ime_mode_scope');
  document.querySelectorAll<HTMLInputElement>('input[name="input-method"]').forEach((radio) => {
    radio.addEventListener('change', () => {
      if (applyingInputConfig) return;
      if (!radio.checked || (radio.value !== 'quanpin' && radio.value !== 'shuangpin' && radio.value !== 'wubi')) {
        return;
      }
      const schema = radio.value as InputScheme;
      updateInputConfig('input.schema', schema);
      updateCandidatePreviewHelpcode({ input_schema: schema });
    });
  });

  setupDropdownMenu(
    'shuangpinSchemeBtn',
    'shuangpinSchemeMenu',
    'changeShuangpinScheme',
    true,
    'input.shuangpin_schema'
  );
  setupDropdownMenu('wubiSchemeBtn', 'wubiSchemeMenu', 'changeWubiScheme', true, 'input.wubi_schema');

  setupPageOptions();
  setupFrequencyOptions();
  setupToggleButton('wordToCharacterToggleBtn', (active) => {
    updateConfig('input.word_to_character', active);
  });
  document.querySelectorAll<HTMLInputElement>('input[name="word-to-character-keys"]').forEach((radio) => {
    radio.addEventListener('change', () => {
      if (radio.checked && !radio.disabled) updateConfig('input.word_to_character_keys', radio.value);
    });
  });
  setupToggleButton('alwaysEnglishPunctuationToggleBtn', (active) => {
    if (active) {
      updateConfig('input.punctuation_lock', 'english');
    } else {
      updateConfig('input.punctuation_lock', 'follow');
    }
  });
  setupToggleButton('smartPunctuationToggleBtn', (active) => {
    updateConfig('input.smart_punctuation', active);
  });
  setupToggleButton('autocorrectTranspositionToggleBtn', (active) => {
    updateConfig('quanpin.autocorrect_transposition', active);
  });
  setupToggleButton('autocorrectNeighborToggleBtn', (active) => {
    updateConfig('quanpin.autocorrect_neighbor', active);
  });
  setupFuzzySection();
  setupToggleButton('smartPunctuationRepeatToChineseToggleBtn', (active) => {
    updateConfig('input.smart_punctuation_repeat_to_chinese', active);
  });
  setupToggleButton('pairedPunctuationToggleBtn', (active) => {
    updateConfig('input.paired_punctuation', active);
  });
  setupToggleButton('zhEnToggleBtn', (active) => {
    syncZhEnMixedInputOptionsEnabled(active);
    updateConfig('general.cn_en_mixed_input', active);
  });
  setupToggleButton('candidateTranslationsToggleBtn', (active) => {
    syncCandidateTranslationOptions(active);
    updateConfig('general.candidate_translations', active);
  });
  setupDropdownMenu(
    'translationTargetLanguageBtn',
    'translationTargetLanguageMenu',
    '',
    true,
    'tencent_tmt.target_language'
  );
  setupDropdownMenu('translationProviderBtn', 'translationProviderMenu', '', true);
  document.getElementById('translationProviderMenu')?.addEventListener('click', (event: Event) => {
    const item = (event.target as HTMLElement | null)?.closest<HTMLElement>('.dropdown-item');
    if (!item) return;
    const useCustom = item.dataset.value === 'custom';
    syncTranslationProviderView(useCustom);
    updateConfig('custom_translation.enabled', useCustom);
  });
  const translationFields: Record<string, string> = {
    tencentTmtSecretId: 'tencent_tmt.secret_id',
    tencentTmtSecretKey: 'tencent_tmt.secret_key',
    customTranslationEndpoint: 'custom_translation.endpoint',
    customTranslationApiKey: 'custom_translation.api_key'
  };
  Object.entries(translationFields).forEach(([id, path]) => {
    const input = document.getElementById(id) as HTMLInputElement | null;
    input?.addEventListener('change', () => {
      input.value = input.value.trim();
      updateConfig(path, input.value);
      syncCandidateTranslationWarning();
    });
  });
  setupSecretVisibility('tencentTmtSecretKey', 'tencentTmtSecretKeyVisibility', 'SecretKey');
  setupSecretVisibility('customTranslationApiKey', 'customTranslationApiKeyVisibility', 'API Key');
  setupDropdownMenu(
    'zhEnTriggerLengthBtn',
    'zhEnTriggerLengthMenu',
    '',
    true,
    'general.cn_en_mixed_input_min_chars',
    Number
  );
  setupToggleButton('emojiMixedInputToggleBtn', (active) => {
    updateConfig('general.emoji_mixed_input', active);
  });
  setupToggleButton('kaomojiMixedInputToggleBtn', (active) => {
    updateConfig('general.kaomoji_mixed_input', active);
  });
  setupToggleButton('cloudCandidatesToggleBtn', (active) => {
    updateConfig('general.cloud_candidates', active);
  });
}

function setupFrequencyOptions(): void {
  setupDropdownMenu('frequencyModeBtn', 'frequencyModeMenu', '', true, 'frequency_adjustment.mode');
  setupDropdownMenu('frequencyTriggerCountBtn', 'frequencyTriggerCountMenu', '', true,
    'frequency_adjustment.trigger_count', Number);
  setupDropdownMenu('frequencyLinearStepBtn', 'frequencyLinearStepMenu', '', true,
    'frequency_adjustment.linear_step', Number);
}

function setupPageOptions(): void {
  document.querySelectorAll<HTMLInputElement>('input[name="page-method"]').forEach((checkbox) => {
    checkbox.addEventListener('change', () => {
      const configPaths: Record<string, string> = {
        minus: 'general.paging_minus_equal',
        comma: 'general.paging_comma_period',
        brackets: 'general.paging_brackets',
        tab: 'general.paging_tab',
        page: 'general.paging_page_up_down',
        arrow: 'general.candidate_arrow_navigation',
        wheel: 'general.paging_mouse_wheel'
      };
      const path = configPaths[checkbox.value];
      if (path) updateConfig(path, checkbox.checked);
    });
  });
}

// 模糊音分区：折叠头 + 总开关 + 11 规则复选。折叠交互仿 appearance.ts 的主题模式：
// aria-expanded 驱动 chevron 与容器显隐，默认收起且不跨会话持久化。
function setupFuzzySection(): void {
  setupToggleButton('fuzzyPinyinToggleBtn', (active) => {
    updateConfig('input.fuzzy_pinyin', active);
    setFuzzyRuleOptionsDisabled(!active);
  });
  const fuzzyExpand = document.getElementById('fuzzyExpand');
  const fuzzyDetails = document.getElementById('fuzzyDetails');
  fuzzyExpand?.addEventListener('click', () => {
    const expanded = fuzzyExpand.getAttribute('aria-expanded') !== 'true';
    fuzzyExpand.setAttribute('aria-expanded', String(expanded));
    fuzzyDetails?.classList.toggle('open', expanded);
  });
  setupFuzzyRuleOptions();
}

// 模糊音 11 键同名同前缀：checkbox value 直接是 [input] 段的配置键。
function setupFuzzyRuleOptions(): void {
  document.querySelectorAll<HTMLInputElement>('input[name="fuzzy-rule"]').forEach((checkbox) => {
    checkbox.addEventListener('change', () => {
      if (checkbox.value.startsWith('fuzzy_')) updateConfig(`input.${checkbox.value}`, checkbox.checked);
    });
  });
}
