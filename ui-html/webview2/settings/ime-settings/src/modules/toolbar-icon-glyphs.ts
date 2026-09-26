// Preview counterpart of the glyph fallback script in ftb/*.html: the toolbar markup is copied in through DOMParser, so its inline script never runs here. Segoe Fluent Icons ships with Windows 11 only and older Segoe MDL2 Assets builds lack some IME codepoints; Chromium silently substitutes a font, so a missing glyph would preview as a blank box (issue #232). U+10FFFD is in no font, which makes its rendering the reference for "missing".
const missingGlyphCache = new Map<string, boolean>();
let renderGlyph: ((text: string) => Uint8ClampedArray) | null | undefined;

function glyphRenderer(): ((text: string) => Uint8ClampedArray) | null {
  if (renderGlyph !== undefined) return renderGlyph;
  const canvas = document.createElement('canvas');
  canvas.width = 32;
  canvas.height = 32;
  let context: CanvasRenderingContext2D | null = null;
  try {
    context = canvas.getContext('2d', { willReadFrequently: true });
  } catch {
    context = null;
  }
  renderGlyph = context ? (text: string) => {
    context!.clearRect(0, 0, 32, 32);
    context!.font = '24px "Segoe Fluent Icons", "Segoe MDL2 Assets"';
    context!.textBaseline = 'top';
    context!.fillText(text, 4, 4);
    return context!.getImageData(0, 0, 32, 32).data;
  } : null;
  return renderGlyph;
}

function isGlyphMissing(text: string): boolean {
  const cached = missingGlyphCache.get(text);
  if (cached !== undefined) return cached;
  const render = glyphRenderer();
  if (!render) return false;
  const missing = render('\u{10FFFD}');
  const pixels = render(text);
  const result = pixels.every((value, index) => value === missing[index]);
  missingGlyphCache.set(text, result);
  return result;
}

export function applyToolbarIconGlyphFallbacks(root: ParentNode): void {
  root.querySelectorAll<HTMLElement>('.icon-glyph[data-fallback]').forEach((node) => {
    if (!isGlyphMissing(node.textContent || '')) return;
    node.textContent = node.dataset.fallback || '';
    node.classList.add('is-fallback');
  });
}
