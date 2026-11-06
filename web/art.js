// The game's own pictures for the lobby: stage previews and fighter portraits, decoded from the
// data image the engine already holds (ssb64.data). Nothing is shipped twice and nothing is drawn by hand.
// Layout (tools/port/src/bin/packdata.rs, write_image): a table of 12-byte entries, file data after it.
// Pointers inside a file are chain words, (next << 16) | target, both in words. Texels and palettes keep
// the N64's big-endian byte order, the rest of a file is little-endian.
const FILES = { portraits: 0x13, maps: 0x1e };
// Sprite offsets from include/reloc_data.us.h, in the order the lobby lists fighters and stages.
const PORTRAIT = [0x4728, 0xd068, 0x8bc8, 0xae18, 0x6978, 0x11508, 0x13758, 0x19e48, 0xf2b8, 0x159a8, 0x1c098, 0x17bf8]; // Mario Fox DK Samus Luigi Link Yoshi Falcon Kirby Pikachu Jigglypuff Ness
const STAGE_NAMES = ['PeachsCastle', 'SectorZ', 'CongoJungle', 'PlanetZebes', 'HyruleCastle', 'YoshisIsland', 'DreamLand', 'SaffronCity', 'MushroomKingdom'];
const x5 = (v) => (v << 3) | (v >> 2);

export class Art {
  constructor(heap, base) {
    this.h = heap; this.base = base; this.interleave = true;
    this.dv = new DataView(heap.buffer, heap.byteOffset, heap.byteLength);
    this.cache = new Map();
  }
  file(id) {
    const e = this.base + id * 12, n = this.dv.getUint32.bind(this.dv);
    const hi = this.base + this.tableEnd();
    return hi + n(e, true);
  }
  tableEnd() {
    if (this._hi) return this._hi;
    // The table runs to the first entry's start: (n + 1) * 12 bytes.
    let i = 0;
    while (this.dv.getUint16(this.base + i * 12 + 4, true) !== 0xFFFF || this.dv.getUint16(this.base + i * 12 + 6, true) !== 0) i++;
    this._hi = (i + 1) * 12;
    return this._hi;
  }
  ptr(f, off) { const w = this.dv.getUint32(f + off, true); return f + (w & 0xFFFF) * 4; }
  // One sprite as RGBA: its bitmaps are strips stacked top to bottom.
  sprite(fileId, off) {
    const key = fileId + ':' + off;
    if (this.cache.has(key)) return this.cache.get(key);
    const f = this.file(fileId), s = f + off, d = this.dv, H = this.h;
    const width = d.getInt16(s + 4, true), nb = d.getInt16(s + 40, true), fmt = H[s + 48], siz = H[s + 49];
    const lut = d.getUint32(s + 32, true) ? this.ptr(f, off + 32) : 0;
    const bm = this.ptr(f, off + 52);
    const strips = [];
    let height = 0;
    for (let i = 0; i < nb; i++) {
      const b = bm + i * 16, w = d.getInt16(b, true), wi = d.getInt16(b + 2, true), rows = d.getInt16(b + 12, true);
      if (!w) break;
      strips.push({ w, wi, rows, buf: this.ptr(f, (b - f) + 8) });
      height += rows;
    }
    const out = new Uint8ClampedArray(width * height * 4);
    let y0 = 0;
    for (const st of strips) {
      for (let y = 0; y < st.rows; y++) for (let x = 0; x < Math.min(st.w, width); x++) {
        let r = 255, g = 255, b = 255, a = 255;
        const i = y * st.wi + x;
        // Odd rows are stored the way TMEM wants them: the two 32-bit halves of each 64-bit word swapped.
        const sw = this.swapRow(y, siz);
        if (siz === 0) { const t = (H[st.buf + ((i >> 1) ^ sw)] >> ((i & 1) ? 0 : 4)) & 15; [r, g, b, a] = this.tex4(fmt, t, lut); }
        else if (siz === 1) { const t = H[st.buf + (i ^ sw)]; [r, g, b, a] = this.tex8(fmt, t, lut); }
        else if (siz === 2) { const q = (i * 2) ^ sw, c = (H[st.buf + q] << 8) | H[st.buf + q + 1]; if (fmt === 3) { r = g = b = c >> 8; a = c & 255; } else { r = x5(c >> 11); g = x5((c >> 6) & 31); b = x5((c >> 1) & 31); a = (c & 1) ? 255 : 0; } }
        else { const p = st.buf + ((i * 4) ^ sw); r = H[p]; g = H[p + 1]; b = H[p + 2]; a = H[p + 3]; }
        const o = ((y0 + y) * width + x) * 4; out[o] = r; out[o + 1] = g; out[o + 2] = b; out[o + 3] = a;
      }
      y0 += st.rows;
    }
    const img = { width, height, data: out };
    this.cache.set(key, img);
    return img;
  }
  // Odd rows of a bitmap are stored for TMEM: 64-bit words with their halves swapped. A 32-bit image's
  // texels sit in TMEM's two banks, so there the swap is a whole 64-bit word (measured: smoothest).
  swapRow(y, siz) { return (y & 1) ? (siz === 3 ? 8 : 4) : 0; }
  pal(lut, t) { const c = (this.h[lut + t * 2] << 8) | this.h[lut + t * 2 + 1]; return [x5(c >> 11), x5((c >> 6) & 31), x5((c >> 1) & 31), (c & 1) ? 255 : 0]; }
  tex4(fmt, t, lut) { if (fmt === 2) return this.pal(lut, t); if (fmt === 3) { const i = t >> 1, v = (i << 5) | (i << 2) | (i >> 1); return [v, v, v, (t & 1) ? 255 : 0]; } const v = (t << 4) | t; return [v, v, v, v]; }
  tex8(fmt, t, lut) { if (fmt === 2) return this.pal(lut, t); if (fmt === 3) { const i = t >> 4, v = (i << 4) | i, a = t & 15; return [v, v, v, (a << 4) | a]; } return [t, t, t, t]; }
  // A data: URL of a picture, so a plain <img> can show it.
  url(img) {
    const c = document.createElement('canvas'); c.width = img.width; c.height = img.height;
    c.getContext('2d').putImageData(new ImageData(img.data, img.width, img.height), 0, 0);
    return c.toDataURL();
  }
  // A fighter's portrait. The game has none for its 1P-only fighters, so those are drawn from the
  // fighter they copy: Metal Mario in steel, a Fighting Polygon in the polygons' violet, Giant DK as DK.
  portrait(i, look = null) {
    const src = this.sprite(FILES.portraits, PORTRAIT[i]);
    if (!look || look === 'giant') return this.url(src);
    const data = new Uint8ClampedArray(src.data);
    for (let p = 0; p < data.length; p += 4) {
      const l = (data[p] * 0.3 + data[p + 1] * 0.59 + data[p + 2] * 0.11) / 255;
      if (look === 'metal') { const v = Math.min(1, Math.pow(l, 0.8) * 1.15); data[p] = 70 + v * 175; data[p + 1] = 78 + v * 172; data[p + 2] = 92 + v * 163; }
      else { const v = Math.round(l * 4) / 4; data[p] = 40 + v * 150; data[p + 1] = 20 + v * 80; data[p + 2] = 90 + v * 165; }
    }
    return this.url({ width: src.width, height: src.height, data });
  }
  // A stage's preview by GRKind. The 9 VS stages have the select screen's own; Meta Crystal, Duel Zone
  // and Final Destination have none there, so they show the stage's own wallpaper.
  stage(kind) {
    if (STAGE_OFFSETS[kind] !== undefined) return this.url(this.sprite(FILES.maps, STAGE_OFFSETS[kind]));
    const w = WALLPAPER[kind];
    return w ? this.url(this.sprite(w, 0x26c88)) : '';
  }
}
// Stage preview sprite offsets (llMNMaps<Name>Sprite), by GRKind 0 to 8.
const STAGE_OFFSETS = [0x4d88, 0x5b68, 0x6948, 0x7728, 0x8508, 0x92e8, 0xbc88, 0xa0c8, 0xaea8];
// Wallpaper files (llStage<Name>BackgroundFileID, sprite at 0x26c88), by GRKind: the map files
// src/relocData/269_GRMetalMap.c, 268_GRZakoMap.c and 266_GRLastMap.c name these as their wallpaper.
const WALLPAPER = { 13: 0x62, 14: 0x61, 16: 0x60 };
void STAGE_NAMES;
