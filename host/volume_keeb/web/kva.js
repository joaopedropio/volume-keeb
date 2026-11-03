// The knob's animation format (keymap.c, above anim_parse()): encoding one
// from RGBA frames, and decoding one back for previews. Needs gif.js.
'use strict';

const Kva = (() => {
  const HEADER = 16;

  // RGB565 in the order the panel takes it, as the file stores it.
  const to565 = (r, g, b) => ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  const from565 = v => [((v >> 11) & 31) * 255 / 31 | 0, ((v >> 5) & 63) * 255 / 63 | 0, (v & 31) * 255 / 31 | 0];

  // One frame's runs; prev = the frame before it (null for the first, which
  // must set every pixel).
  function runs(cur, prev, out) {
    const n = cur.length;
    const same = j => prev !== null && cur[j] === prev[j];
    let i = 0;
    while (i < n) {
      if (same(i)) {
        let k = 1;
        while (i + k < n && k < 64 && same(i + k)) k++;
        out.push(0x80 | (k - 1));
        i += k;
        continue;
      }
      let r = 1;
      while (i + r < n && r < 128 && cur[i + r] === cur[i]) r++;
      if (r >= 2) {
        out.push(r - 1, cur[i]);
        i += r;
        continue;
      }
      let k = 1;  // a literal, until something cheaper starts
      while (i + k < n && k < 64) {
        const j = i + k;
        if (same(j) && j + 1 < n && same(j + 1)) break;
        if (j + 1 < n && cur[j] === cur[j + 1]) break;
        k++;
      }
      out.push(0xC0 | (k - 1));
      for (let j = 0; j < k; j++) out.push(cur[i + j]);
      i += k;
    }
  }

  // frames: opaque RGBA buffers of w*h; delays in ms; scale: the screen
  // pixels per art pixel. -> {bytes, frames, colors, sizes}: sizes[f] is
  // what frames[f] takes in the file (0 for one merged into the one before).
  function encode(w, h, scale, frames, delays) {
    const reduced = frames.map(d => {
      const o = new Uint8ClampedArray(d.length);
      for (let i = 0; i < d.length; i += 4) {
        o[i] = d[i] & 0xF8; o[i + 1] = d[i + 1] & 0xFC; o[i + 2] = d[i + 2] & 0xF8; o[i + 3] = 255;
      }
      return o;
    });
    const pal = Gif.palette(reduced, 256);
    const idx = reduced.map(d => {
      const a = new Uint8Array(w * h);
      for (let i = 0, o = 0; o < a.length; i += 4, o++) a[o] = pal.lookup.get((d[i] << 16) | (d[i + 1] << 8) | d[i + 2]) || 0;
      return a;
    });
    // A frame that repeats the one before just makes that one last longer.
    const kept = [], kd = [], from = [];
    idx.forEach((a, f) => {
      const last = kept[kept.length - 1];
      if (last && a.every((v, i) => v === last[i])) kd[kd.length - 1] += delays[f];
      else { kept.push(a); kd.push(delays[f]); from.push(f); }
    });
    const sizes = new Array(frames.length).fill(0);
    const out = [...'KVA1'].map(c => c.charCodeAt(0));
    const u16 = v => out.push(v & 255, (v >> 8) & 255);
    u16(w); u16(h);
    out.push(scale, pal.colors.length - 1);
    u16(kept.length);
    out.push(0, 0, 0, 0);
    for (const c of pal.colors) {
      const v = to565(c >> 16, (c >> 8) & 255, c & 255);
      out.push(v >> 8, v & 255);
    }
    const data = [];
    kept.forEach((a, f) => {
      data.length = 0;
      runs(a, f ? kept[f - 1] : null, data);
      u16(Math.min(65535, Math.max(1, Math.round(kd[f]))));
      const n = data.length;
      out.push(n & 255, (n >> 8) & 255, (n >> 16) & 255, n >>> 24);
      for (const b of data) out.push(b);
      sizes[from[f]] = 6 + n;
    });
    return { bytes: new Uint8Array(out), frames: kept.length, colors: pal.colors.length, sizes };
  }

  // -> {w, h, scale, frames: [{rgba, delay}]}, the art pixels as the knob has them.
  function decode(b) {
    if (b.length < HEADER || String.fromCharCode(...b.subarray(0, 4)) !== 'KVA1') throw new Error('not an animation');
    const u16 = p => b[p] | (b[p + 1] << 8);
    const w = u16(4), h = u16(6), scale = b[8], ncol = b[9] + 1, count = u16(10);
    const pal = [];
    for (let i = 0; i < ncol; i++) pal.push(from565((b[HEADER + 2 * i] << 8) | b[HEADER + 2 * i + 1]));
    const rgba = new Uint8ClampedArray(w * h * 4);
    const frames = [];
    let p = HEADER + 2 * ncol;
    for (let f = 0; f < count && p + 6 <= b.length; f++) {
      const delay = u16(p), n = b[p + 2] | (b[p + 3] << 8) | (b[p + 4] << 16) | (b[p + 5] << 24);
      p += 6;
      const end = Math.min(b.length, p + n);
      let px = 0;
      const put = c => { const o = px++ * 4; if (o < rgba.length) { rgba[o] = pal[c][0]; rgba[o + 1] = pal[c][1]; rgba[o + 2] = pal[c][2]; rgba[o + 3] = 255; } };
      while (p < end) {
        const op = b[p++];
        const k = (op < 0x80 ? op & 0x7F : op & 0x3F) + 1;
        if (op < 0x80) { const c = b[p++] % ncol; for (let j = 0; j < k; j++) put(c); }
        else if (op < 0xC0) px += k;
        else for (let j = 0; j < k; j++) put(b[p++] % ncol);
      }
      p = end;
      frames.push({ rgba: rgba.slice(), delay: Math.max(20, delay) });
    }
    return { w, h, scale, frames };
  }

  // Plays a decoded animation on a canvas the size of the knob's screen,
  // placed as the knob places it. Stops once the canvas leaves the page.
  function play(canvas, anim) {
    const W = 428, H = 142;
    canvas.width = W;
    canvas.height = H;
    const ctx = canvas.getContext('2d');
    ctx.imageSmoothingEnabled = false;
    const art = document.createElement('canvas');
    art.width = anim.w;
    art.height = anim.h;
    const actx = art.getContext('2d');
    const images = anim.frames.map(f => new ImageData(f.rgba, anim.w, anim.h));
    const sw = anim.w * anim.scale, sh = anim.h * anim.scale;
    const x0 = sw < W ? Math.floor((W - sw) / 2) : 0, y0 = sh < H ? Math.floor((H - sh) / 2) : 0;
    let f = 0;
    const step = () => {
      actx.putImageData(images[f], 0, 0);
      ctx.fillStyle = '#000';
      ctx.fillRect(0, 0, W, H);
      ctx.drawImage(art, x0, y0, sw, sh);
      const delay = anim.frames[f].delay;
      f = (f + 1) % images.length;
      if (images.length > 1) setTimeout(() => { if (canvas.isConnected) step(); }, delay);
    };
    step();
  }

  return { encode, decode, play };
})();
