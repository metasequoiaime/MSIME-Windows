import { applyDropdownValue, applyToggleState, setupDropdownMenu, setupToggleButton } from './shared';
import { updateConfig } from './config-sync';
import { setupCredentialTest } from './credential-test';

type ProviderDefaults = { endpoint: string; model: string };

const fields: Record<string, string> = {
  aiToken: 'token', aiEndpoint: 'endpoint', aiModel: 'model'
};

const PROVIDER_DEFAULTS: Record<string, ProviderDefaults> = {
  deepseek: {
    endpoint: 'https://api.deepseek.com/chat/completions',
    model: 'deepseek-v4-flash'
  },
  openai: {
    endpoint: 'https://api.openai.com/v1/chat/completions',
    model: 'gpt-4o-mini'
  },
  siliconflow: {
    endpoint: 'https://api.siliconflow.cn/v1/chat/completions',
    model: 'Qwen/Qwen3-8B'
  },
  groq: {
    endpoint: 'https://api.groq.com/openai/v1/chat/completions',
    model: 'llama-3.3-70b-versatile'
  }
};

const PROVIDERS = ['deepseek', 'openai', 'siliconflow', 'groq'] as const;
let tokens: Record<string, string> = {};
let endpoints: Record<string, string> = {};
let models: Record<string, string> = {};
let currentProvider = 'deepseek';
let currentPromptId = 'custom_1';
let customPrompts: Record<string, string> = { custom_1: '', custom_2: '', custom_3: '' };

function applyProviderFields(provider: string): void {
  const defaults = PROVIDER_DEFAULTS[provider];
  if (!defaults) return;
  const endpoint = document.getElementById('aiEndpoint') as HTMLInputElement | null;
  const model = document.getElementById('aiModel') as HTMLInputElement | null;
  if (endpoint) endpoint.value = endpoints[provider] ?? defaults.endpoint;
  if (model) {
    model.value = models[provider] ?? defaults.model;
    model.placeholder = defaults.model;
  }
}

function readProviderMap(raw: unknown, provider: string, legacyValue: string): Record<string, string> {
  const result: Record<string, string> = {};
  if (raw && typeof raw === 'object') {
    Object.entries(raw as Record<string, unknown>).forEach(([key, value]) => {
      if (typeof value === 'string') result[key] = value;
    });
  }
  if (legacyValue && !result[provider]) result[provider] = legacyValue;
  return result;
}

function switchProvider(provider: string): void {
  const token = document.getElementById('aiToken') as HTMLInputElement | null;
  if (token) {
    tokens[currentProvider] = token.value.trim();
    updateConfig(`ai_assistant.token_${currentProvider}`, tokens[currentProvider]);
  }
  for (const [id, key, slots] of [
    ['aiEndpoint', 'endpoint', endpoints], ['aiModel', 'model', models]
  ] as const) {
    const input = document.getElementById(id) as HTMLInputElement | null;
    if (input) {
      slots[currentProvider] = input.value;
      updateConfig(`ai_assistant.${key}`, input.value);
    }
  }
  currentProvider = provider;
  if (token) token.value = tokens[provider] ?? '';
  updateConfig('ai_assistant.provider', provider);
  applyProviderFields(provider);
}

function switchPrompt(id: string): void {
  const prompt = document.getElementById('aiPrompt') as HTMLTextAreaElement | null;
  if (prompt) {
    customPrompts[currentPromptId] = prompt.value;
    updateConfig(`ai_assistant.prompt_${currentPromptId}`, prompt.value);
  }
  currentPromptId = id;
  if (prompt) prompt.value = customPrompts[id] ?? '';
}

export function setupAiSettings(): void {
  setupToggleButton('aiEnabled', value => updateConfig('ai_assistant.enabled', value));
  setupTokenVisibilityToggle();
  setupCredentialTest('aiCredentialTestButton', 'aiCredentialTestStatus', () => 'ai.assistant', () => {
    const defaults = PROVIDER_DEFAULTS[currentProvider];
    const value = (id: string) =>
      (document.getElementById(id) as HTMLInputElement | null)?.value.trim() ?? '';
    return {
      provider: currentProvider,
      token: value('aiToken'),
      endpoint: value('aiEndpoint') || defaults?.endpoint || '',
      model: value('aiModel') || defaults?.model || ''
    };
  });
  setupDropdownMenu('aiProviderBtn', 'aiProviderMenu', 'changeAiProvider', true);
  setupDropdownMenu('aiPromptSlotBtn', 'aiPromptSlotMenu', 'changeAiPromptSlot', true,
    'ai_assistant.prompt_id');
  document.getElementById('aiProviderMenu')?.addEventListener('click', (event) => {
    const item = (event.target as HTMLElement | null)?.closest('.dropdown-item') as HTMLElement | null;
    const provider = item?.dataset.value;
    if (provider) switchProvider(provider);
  });
  document.getElementById('aiPromptSlotMenu')?.addEventListener('click', (event) => {
    const item = (event.target as HTMLElement | null)?.closest('.dropdown-item') as HTMLElement | null;
    const id = item?.dataset.value;
    if (id) switchPrompt(id);
  });
  Object.entries(fields).forEach(([id, key]) => {
    const element = document.getElementById(id) as HTMLInputElement | HTMLTextAreaElement | null;
    element?.addEventListener('change', () => {
      if (id === 'aiToken') {
        tokens[currentProvider] = element.value.trim();
        updateConfig(`ai_assistant.token_${currentProvider}`, tokens[currentProvider]);
        return;
      }
      updateConfig(`ai_assistant.${key}`, element.value);
    });
  });
  const limit = document.getElementById('aiCandidateLimit') as HTMLInputElement | null;
  limit?.addEventListener('change', () => {
    const value = Math.max(1, Math.min(10, Number.parseInt(limit.value, 10) || 3));
    limit.value = String(value);
    updateConfig('ai_assistant.candidate_limit', value);
  });
  const prompt = document.getElementById('aiPrompt') as HTMLTextAreaElement | null;
  prompt?.addEventListener('change', () => {
    customPrompts[currentPromptId] = prompt.value;
    updateConfig(`ai_assistant.prompt_${currentPromptId}`, prompt.value);
  });
}

function setupTokenVisibilityToggle(): void {
  const token = document.getElementById('aiToken') as HTMLInputElement | null;
  const toggle = document.getElementById('aiTokenVisibility') as HTMLButtonElement | null;
  if (!token || !toggle) return;

  toggle.addEventListener('click', () => {
    const shouldShow = token.type === 'password';
    token.type = shouldShow ? 'text' : 'password';
    toggle.setAttribute('aria-pressed', String(shouldShow));

    const label = shouldShow ? '隐藏 API Token' : '显示 API Token';
    toggle.setAttribute('aria-label', label);
    toggle.title = label;
  });
}

export function applyAiConfig(config: Record<string, unknown>): void {
  Object.entries(fields).forEach(([id, key]) => {
    const element = document.getElementById(id) as HTMLInputElement | HTMLTextAreaElement | null;
    if (element && typeof config[key] === 'string') element.value = config[key] as string;
  });
  const limit = document.getElementById('aiCandidateLimit') as HTMLInputElement | null;
  if (limit && typeof config.candidate_limit === 'number') limit.value = String(config.candidate_limit);
  if (typeof config.enabled === 'boolean') applyToggleState('aiEnabled', config.enabled);
  currentProvider = typeof config.provider === 'string' ? config.provider : 'deepseek';
  tokens = readProviderMap(config.tokens, currentProvider, typeof config.token === 'string' ? config.token : '');
  endpoints = readProviderMap(config.endpoints, currentProvider, typeof config.endpoint === 'string' ? config.endpoint : '');
  models = readProviderMap(config.models, currentProvider, typeof config.model === 'string' ? config.model : '');
  PROVIDERS.forEach((provider) => {
    const slot = config[`token_${provider}`];
    if (typeof slot === 'string' && slot) tokens[provider] = slot;
  });
  const token = document.getElementById('aiToken') as HTMLInputElement | null;
  if (token) token.value = tokens[currentProvider] ?? '';
  applyDropdownValue('aiProviderBtn', 'aiProviderMenu', currentProvider);
  const defaults = PROVIDER_DEFAULTS[currentProvider];
  const model = document.getElementById('aiModel') as HTMLInputElement | null;
  if (model && defaults) model.placeholder = defaults.model;
  currentPromptId = typeof config.prompt_id === 'string' ? config.prompt_id : 'custom_1';
  customPrompts = {
    custom_1: typeof config.prompt_custom_1 === 'string'
      ? config.prompt_custom_1
      : (typeof config.prompt === 'string' ? config.prompt : ''),
    custom_2: typeof config.prompt_custom_2 === 'string' ? config.prompt_custom_2 : '',
    custom_3: typeof config.prompt_custom_3 === 'string' ? config.prompt_custom_3 : ''
  };
  applyDropdownValue('aiPromptSlotBtn', 'aiPromptSlotMenu', currentPromptId);
  const prompt = document.getElementById('aiPrompt') as HTMLTextAreaElement | null;
  if (prompt) prompt.value = customPrompts[currentPromptId] ?? '';
}
