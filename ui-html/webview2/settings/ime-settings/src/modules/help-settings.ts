import { updateConfig } from './config-sync';

// 网络代理设置（[network] 段）。代理的权威在宿主：地址由 Server 规范化后随快照回放，
// 页面只做与宿主同规则的格式预检，避免把坏值发过去换来一次「设置保存失败」。

const PROXY_HINT = '仅支持 HTTP 代理，格式为「主机:端口」，如 127.0.0.1:7890；Clash、v2rayN 等客户端的 HTTP/混合端口均可';
const PROXY_PATTERN = /^(?:http:\/\/)?(\[[0-9a-f:.]+\]|[a-z0-9._-]+):(\d{1,5})\/*$/i;

let currentMode = 'system';

export function isValidProxyServer(value: string): boolean {
  const match = PROXY_PATTERN.exec(value.trim());
  if (!match) return false;
  const port = Number(match[2]);
  return port >= 1 && port <= 65535;
}

function proxyInput(): HTMLInputElement | null {
  return document.getElementById('networkProxyServer') as HTMLInputElement | null;
}

function setStatus(message: string, isError: boolean): void {
  const status = document.getElementById('networkProxyStatus');
  if (!status) return;
  status.textContent = message;
  status.classList.toggle('is-error', isError);
}

function refreshServerState(): void {
  const input = proxyInput();
  if (!input) return;
  const custom = currentMode === 'custom';
  input.disabled = !custom;
  if (custom && !input.value.trim()) {
    setStatus('请填写代理地址；未填写时按「跟随系统代理设置」处理', true);
  } else if (!input.classList.contains('is-invalid')) {
    setStatus(PROXY_HINT, false);
  }
}

export function setupHelpSettings(): void {
  document.querySelectorAll<HTMLInputElement>('input[name="network-proxy-mode"]').forEach((radio) => {
    radio.addEventListener('change', () => {
      if (!radio.checked) return;
      currentMode = radio.value;
      updateConfig('network.proxy_mode', radio.value);
      refreshServerState();
    });
  });
  const input = proxyInput();
  input?.addEventListener('change', () => {
    const value = input.value.trim();
    if (value && !isValidProxyServer(value)) {
      input.classList.add('is-invalid');
      setStatus('代理地址格式不正确：应为「主机:端口」，如 127.0.0.1:7890（不支持 socks5://）', true);
      return;
    }
    input.classList.remove('is-invalid');
    updateConfig('network.proxy_server', value);
    refreshServerState();
  });
}

export function applyNetworkConfig(config: Record<string, unknown>): void {
  if (config.proxy_mode === 'system' || config.proxy_mode === 'none' || config.proxy_mode === 'custom') {
    currentMode = config.proxy_mode;
    document.querySelectorAll<HTMLInputElement>('input[name="network-proxy-mode"]').forEach((radio) => {
      radio.checked = radio.value === currentMode;
    });
  }
  const input = proxyInput();
  // 正在编辑时不回填：快照随任意配置变更到达，覆盖输入框会吞掉用户还没提交的字。
  if (input && typeof config.proxy_server === 'string' && document.activeElement !== input) {
    input.value = config.proxy_server;
    input.classList.remove('is-invalid');
  }
  refreshServerState();
}
