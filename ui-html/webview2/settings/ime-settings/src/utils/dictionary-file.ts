function decodeUtf8(bytes: Uint8Array, fatal: boolean): string {
  return new TextDecoder('utf-8', { fatal }).decode(bytes);
}

export function decodeDictionaryBytes(bytes: Uint8Array): string {
  if (bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) {
    return decodeUtf8(bytes.subarray(3), true);
  }
  if (bytes.length >= 2 && bytes[0] === 0xff && bytes[1] === 0xfe) {
    return new TextDecoder('utf-16le').decode(bytes.subarray(2));
  }
  if (bytes.length >= 2 && bytes[0] === 0xfe && bytes[1] === 0xff) {
    return new TextDecoder('utf-16be').decode(bytes.subarray(2));
  }
  try {
    return decodeUtf8(bytes, true);
  } catch {
    for (const label of ['gb18030', 'gbk']) {
      try {
        return new TextDecoder(label).decode(bytes);
      } catch {
        continue;
      }
    }
    return decodeUtf8(bytes, false);
  }
}

export async function readDictionaryFile(file: File): Promise<string> {
  return decodeDictionaryBytes(new Uint8Array(await file.arrayBuffer()));
}
