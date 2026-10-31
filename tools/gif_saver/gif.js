// GIF decoding (every frame composited to full RGBA, like a browser shows it),
// palette building (median cut) and GIF encoding. No dependencies.
'use strict';

const Gif = (() => {
  const u16 = (b, p) => b[p] | (b[p + 1] << 8);

  function isGif(b) {
    return b.length > 6 && b[0] === 0x47 && b[1] === 0x49 && b[2] === 0x46;
  }

  function skipSubBlocks(b, p) {
    while (p < b.length && b[p]) p += b[p] + 1;
    return p + 1;
  }

  function lzwDecode(data, minSize, npix) {
    const out = new Uint8Array(npix);
    const clear = 1 << minSize, eoi = clear + 1;
    const prefix = new Int16Array(4096), suffix = new Uint8Array(4096), first = new Uint8Array(4096);
    const stack = new Uint8Array(4097);
    for (let i = 0; i < clear; i++) { prefix[i] = -1; suffix[i] = i; first[i] = i; }
    let size = minSize + 1, mask = (1 << size) - 1, next = eoi + 1, old = -1;
    let acc = 0, bits = 0, pos = 0, op = 0;
    while (op < npix) {
      while (bits < size) {
        if (pos >= data.length) return out;
        acc |= data[pos++] << bits;
        bits += 8;
      }
      let code = acc & mask;
      acc >>>= size;
      bits -= size;
      if (code === clear) { size = minSize + 1; mask = (1 << size) - 1; next = eoi + 1; old = -1; continue; }
      if (code === eoi) break;
      if (old === -1) {
        if (code >= clear) continue;
        out[op++] = code;
        old = code;
        continue;
      }
      const inCode = code;
      let sp = 0;
      if (code >= next) { stack[sp++] = first[old]; code = old; }
      while (code >= clear) { stack[sp++] = suffix[code]; code = prefix[code]; }
      stack[sp++] = code;
      if (next < 4096) {
        prefix[next] = old;
        suffix[next] = code;
        first[next] = first[old];
        next++;
        if (next === mask + 1 && size < 12) { size++; mask = (1 << size) - 1; }
      }
      while (sp > 0 && op < npix) out[op++] = stack[--sp];
      old = inCode;
    }
    return out;
  }

  // -> {w, h, frames: [{rgba: Uint8ClampedArray (w*h*4), delay: ms}]}
  function decode(b) {
    if (!isGif(b)) throw new Error('not a GIF');
    const w = u16(b, 6), h = u16(b, 8), flags = b[10];
    let p = 13, gct = null;
    if (flags & 0x80) { const n = 2 << (flags & 7); gct = b.subarray(p, p + 3 * n); p += 3 * n; }
    const canvas = new Uint8ClampedArray(w * h * 4);
    const frames = [];
    let gce = { delay: 0, trans: -1, disposal: 0 };
    let dispose = null;  // what the previous frame asked for before the next one draws
    while (p < b.length) {
      const block = b[p++];
      if (block === 0x3B) break;
      if (block === 0x21) {
        const label = b[p++];
        if (label === 0xF9 && b[p] >= 4) {
          const f = b[p + 1];
          gce = { disposal: (f >> 2) & 7, delay: u16(b, p + 2) * 10, trans: f & 1 ? b[p + 4] : -1 };
        }
        p = skipSubBlocks(b, p);
        continue;
      }
      if (block !== 0x2C) break;
      const fx = u16(b, p), fy = u16(b, p + 2), fw = u16(b, p + 4), fh = u16(b, p + 6), f = b[p + 8];
      p += 9;
      let ct = gct;
      if (f & 0x80) { const n = 2 << (f & 7); ct = b.subarray(p, p + 3 * n); p += 3 * n; }
      const minSize = b[p++];
      let len = 0;
      for (let q = p; q < b.length && b[q]; q += b[q] + 1) len += b[q];
      const data = new Uint8Array(len);
      for (let o = 0; p < b.length && b[p]; p += b[p] + 1) { data.set(b.subarray(p + 1, p + 1 + b[p]), o); o += b[p]; }
      p++;
      const idx = lzwDecode(data, minSize, fw * fh);

      if (dispose) {
        if (dispose.kind === 2) {
          for (let y = dispose.y; y < dispose.y + dispose.h && y < h; y++)
            canvas.fill(0, (y * w + dispose.x) * 4, (y * w + Math.min(w, dispose.x + dispose.w)) * 4);
        } else if (dispose.kind === 3 && dispose.saved) {
          canvas.set(dispose.saved);
        }
      }
      const saved = gce.disposal === 3 ? canvas.slice() : null;

      // Interlaced rows come in four passes.
      const rows = new Int32Array(fh);
      if (f & 0x40) {
        let r = 0;
        for (const [start, step] of [[0, 8], [4, 8], [2, 4], [1, 2]])
          for (let y = start; y < fh; y += step) rows[r++] = y;
      } else {
        for (let y = 0; y < fh; y++) rows[y] = y;
      }
      if (ct) {
        for (let r = 0; r < fh; r++) {
          const y = fy + rows[r];
          if (y >= h) continue;
          for (let x = 0; x < fw; x++) {
            const c = idx[r * fw + x];
            if (c === gce.trans || fx + x >= w || c * 3 + 2 >= ct.length) continue;
            const o = (y * w + fx + x) * 4;
            canvas[o] = ct[c * 3]; canvas[o + 1] = ct[c * 3 + 1]; canvas[o + 2] = ct[c * 3 + 2]; canvas[o + 3] = 255;
          }
        }
      }
      // Browsers play delays under 20 ms as 100 ms; do the same.
      frames.push({ rgba: canvas.slice(), delay: gce.delay < 20 ? 100 : gce.delay });
      dispose = { kind: gce.disposal, x: fx, y: fy, w: fw, h: fh, saved };
      gce = { delay: 0, trans: -1, disposal: 0 };
    }
    if (!frames.length) throw new Error('the GIF has no frames');
    return { w, h, frames };
  }

  // Median cut over a histogram (Map rgb -> count) down to n colors.
  function medianCut(hist, n) {
    const keys = [...hist.keys()];
    let boxes = [keys];
    const ch = (k, c) => (k >> (16 - 8 * c)) & 255;
    const range = box => {
      let best = 0, bc = 0;
      for (let c = 0; c < 3; c++) {
        let lo = 255, hi = 0;
        for (const k of box) { const v = ch(k, c); if (v < lo) lo = v; if (v > hi) hi = v; }
        if (hi - lo > best) { best = hi - lo; bc = c; }
      }
      return [best, bc];
    };
    while (boxes.length < n) {
      let pick = -1, score = 0, chan = 0;
      boxes.forEach((box, i) => {
        if (box.length < 2) return;
        const [r, c] = range(box);
        let weight = 0;
        for (const k of box) weight += hist.get(k);
        const s = r * Math.sqrt(weight);
        if (s > score) { score = s; pick = i; chan = c; }
      });
      if (pick < 0) break;
      const box = boxes[pick].sort((a, b) => ch(a, chan) - ch(b, chan));
      let total = 0;
      for (const k of box) total += hist.get(k);
      let acc = 0, cut = 1;
      for (let i = 0; i < box.length - 1; i++) {
        acc += hist.get(box[i]);
        if (acc >= total / 2) { cut = i + 1; break; }
        cut = i + 1;
      }
      boxes.splice(pick, 1, box.slice(0, cut), box.slice(cut));
    }
    return boxes.map(box => {
      let r = 0, g = 0, bl = 0, t = 0;
      for (const k of box) { const c = hist.get(k); r += ch(k, 0) * c; g += ch(k, 1) * c; bl += ch(k, 2) * c; t += c; }
      return (Math.round(r / t) << 16) | (Math.round(g / t) << 8) | Math.round(bl / t);
    });
  }

  // Palette of at most max colors for a list of RGBA buffers (pixels with
  // alpha < 128 are ignored). lookup maps every opaque rgb to its index.
  function palette(buffers, max) {
    const hist = new Map();
    for (const d of buffers)
      for (let i = 0; i < d.length; i += 4) {
        if (d[i + 3] < 128) continue;
        const k = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        hist.set(k, (hist.get(k) || 0) + 1);
      }
    const lookup = new Map();
    if (hist.size <= max) {
      const pal = [...hist.keys()];
      pal.forEach((k, i) => lookup.set(k, i));
      return { colors: pal, lookup };
    }
    const pal = medianCut(hist, max);
    for (const k of hist.keys()) {
      const r = k >> 16, g = (k >> 8) & 255, b = k & 255;
      let best = 0, bd = Infinity;
      for (let i = 0; i < pal.length; i++) {
        const p = pal[i], dr = r - (p >> 16), dg = g - ((p >> 8) & 255), db = b - (p & 255);
        const d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
        if (d < bd) { bd = d; best = i; }
      }
      lookup.set(k, best);
    }
    return { colors: pal, lookup };
  }

  function lzwEncode(idx, minSize) {
    const clear = 1 << minSize, eoi = clear + 1;
    let size = minSize + 1, next = eoi + 1;
    const dict = new Map();
    const bytes = [];
    let acc = 0, nb = 0;
    const emit = c => {
      acc |= c << nb;
      nb += size;
      while (nb >= 8) { bytes.push(acc & 255); acc >>>= 8; nb -= 8; }
    };
    emit(clear);
    let cur = idx[0];
    for (let i = 1; i < idx.length; i++) {
      const k = idx[i], key = (cur << 8) | k, v = dict.get(key);
      if (v !== undefined) { cur = v; continue; }
      emit(cur);
      if (next === 4096) {
        emit(clear);
        dict.clear();
        next = eoi + 1;
        size = minSize + 1;
      } else {
        if (next >= 1 << size) size++;
        dict.set(key, next++);
      }
      cur = k;
    }
    emit(cur);
    emit(eoi);
    if (nb > 0) bytes.push(acc & 255);
    return bytes;
  }

  // Opaque RGBA frames (w*h*4 each) and their delays in ms -> GIF bytes, looping.
  function encode(w, h, frames, delays) {
    const pal = palette(frames, 256);
    let bits = 1;
    while ((1 << bits) < pal.colors.length) bits++;
    const out = [];
    const put16 = v => out.push(v & 255, (v >> 8) & 255);
    out.push(...[...'GIF89a'].map(c => c.charCodeAt(0)));
    put16(w); put16(h);
    out.push(0x80 | 0x70 | (bits - 1), 0, 0);
    for (let i = 0; i < 1 << bits; i++) {
      const c = pal.colors[i] || 0;
      out.push(c >> 16, (c >> 8) & 255, c & 255);
    }
    out.push(0x21, 0xFF, 0x0B, ...[...'NETSCAPE2.0'].map(c => c.charCodeAt(0)), 3, 1, 0, 0, 0);
    const minSize = Math.max(2, bits);
    const idx = new Uint8Array(w * h);
    frames.forEach((d, f) => {
      for (let i = 0, o = 0; o < idx.length; i += 4, o++)
        idx[o] = pal.lookup.get((d[i] << 16) | (d[i + 1] << 8) | d[i + 2]) || 0;
      out.push(0x21, 0xF9, 4, 0x04);
      put16(Math.max(2, Math.round(delays[f] / 10)));
      out.push(0, 0);
      out.push(0x2C); put16(0); put16(0); put16(w); put16(h); out.push(0);
      out.push(minSize);
      const data = lzwEncode(idx, minSize);
      for (let i = 0; i < data.length; i += 255) {
        const n = Math.min(255, data.length - i);
        out.push(n);
        for (let j = 0; j < n; j++) out.push(data[i + j]);
      }
      out.push(0);
    });
    out.push(0x3B);
    return new Uint8Array(out);
  }

  return { isGif, decode, encode, palette };
})();
