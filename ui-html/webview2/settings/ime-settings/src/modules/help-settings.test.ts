import { describe, expect, it } from 'vitest';
import { isValidProxyServer } from './help-settings';

describe('network proxy address precheck', () => {
  // 与 Server 的 NormalizeNetworkProxyServer 同规则：页面拦下的值宿主同样会拒绝。
  it('accepts host:port with an optional http:// prefix', () => {
    expect(isValidProxyServer('127.0.0.1:7890')).toBe(true);
    expect(isValidProxyServer(' http://proxy.example.com:8080/ ')).toBe(true);
    expect(isValidProxyServer('[::1]:1080')).toBe(true);
  });

  it('rejects other schemes, missing or out-of-range ports, and credentials', () => {
    expect(isValidProxyServer('socks5://127.0.0.1:1080')).toBe(false);
    expect(isValidProxyServer('https://127.0.0.1:7890')).toBe(false);
    expect(isValidProxyServer('127.0.0.1')).toBe(false);
    expect(isValidProxyServer('127.0.0.1:0')).toBe(false);
    expect(isValidProxyServer('127.0.0.1:65536')).toBe(false);
    expect(isValidProxyServer('user:pass@127.0.0.1:7890')).toBe(false);
  });
});
