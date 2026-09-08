import { describe, expect, it } from 'vitest';
import { decodeDictionaryBytes } from './dictionary-file';

const utf8 = new TextEncoder();

function utf16LeWithBom(text: string): Uint8Array {
  const payload = new Uint8Array(2 + text.length * 2);
  payload[0] = 0xff;
  payload[1] = 0xfe;
  const view = new DataView(payload.buffer);
  for (let i = 0; i < text.length; i += 1) {
    view.setUint16(2 + i * 2, text.charCodeAt(i), true);
  }
  return payload;
}

function gbkDecoderAvailable(): boolean {
  for (const label of ['gb18030', 'gbk']) {
    try {
      void new TextDecoder(label);
      return true;
    } catch {
      continue;
    }
  }
  return false;
}

describe('decodeDictionaryBytes', () => {
  it('reads utf-8 with and without bom', () => {
    const body = utf8.encode("你好\tni'hao\t1\n");
    expect(decodeDictionaryBytes(body)).toBe("你好\tni'hao\t1\n");
    const bom = new Uint8Array(3 + body.length);
    bom.set([0xef, 0xbb, 0xbf]);
    bom.set(body, 3);
    expect(decodeDictionaryBytes(bom)).toBe("你好\tni'hao\t1\n");
  });

  it('reads utf-16le with bom', () => {
    const text = "阿巴拉提亚云海\ta'ba'la'ti'ya'yun'hai\t13\n";
    expect(decodeDictionaryBytes(utf16LeWithBom(text))).toBe(text);
  });

  it.skipIf(!gbkDecoderAvailable())('falls back from invalid utf-8 to gbk', () => {
    const encoded = new Uint8Array([
      0xc4, 0xe3, 0xba, 0xc3, 0x09, 0x6e, 0x69, 0x27, 0x68, 0x61, 0x6f, 0x09, 0x31,
    ]);
    expect(decodeDictionaryBytes(encoded)).toBe("你好\tni'hao\t1");
  });
});
