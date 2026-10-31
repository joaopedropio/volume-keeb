// Turning upscaled pixel art back into its own pixels, and laying those out
// on the knob's screen. src = {w, h, frames: [{rgba, delay}]} as Gif.decode
// returns it.
'use strict';

const Pixel = (() => {
  // Color difference between neighbouring columns (ex) and rows (ey), summed
  // over a few frames: big where one art pixel ends and the next begins.
  // Small differences (dithering, compression noise) don't count.
  function edgeProfiles(src) {
    const { w, h, frames } = src;
    const ex = new Float64Array(w), ey = new Float64Array(h);
    const step = Math.max(1, Math.floor(frames.length / 8));
    const diff = (d, i, j) => {
      const v = Math.abs(d[i] - d[j]) + Math.abs(d[i + 1] - d[j + 1]) + Math.abs(d[i + 2] - d[j + 2]) +
        (Math.abs(d[i + 3] - d[j + 3]) >> 1);
      return v > 64 ? v : 0;
    };
    for (let f = 0; f < frames.length; f += step) {
      const d = frames[f].rgba;
      for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
          const i = (y * w + x) * 4;
          if (x > 0) ex[x] += diff(d, i, i - 4);
          if (y > 0) ey[y] += diff(d, i, i - w * 4);
        }
    }
    return [ex, ey];
  }

  // The edge energy that falls on a grid of period p, at its best phase.
  function onGrid(E, p) {
    let best = 0, phase = 0;
    for (let ph = 0; ph < p; ph += 0.25) {
      let s = 0;
      for (let x = ph; x < E.length - 0.5; x += p) s += E[Math.round(x)];
      if (s > best) { best = s; phase = ph; }
    }
    return [best, phase];
  }

  // Guess the size of one art pixel in the source and where the grid starts:
  // the biggest square grid that still holds (nearly) every edge of the image.
  // Its halves hold them all too, its doubles only some.
  function detect(src) {
    const [ex, ey] = edgeProfiles(src);
    ex[0] = ey[0] = 0;
    const total = ex.reduce((a, b) => a + b, 0) + ey.reduce((a, b) => a + b, 0);
    const none = { cell: 1, ox: 0, oy: 0, cover: 0 };
    if (!total) return none;
    const score = p => {
      const [sx, ox] = onGrid(ex, p), [sy, oy] = onGrid(ey, p);
      return { p, cover: (sx + sy) / total, ox, oy };
    };
    const n = Math.max(src.w, src.h);
    const maxP = Math.max(2, Math.min(64, Math.min(src.w, src.h) / 2));
    // A grid that's off by d per cell drifts d * n / p by the far edge, so the
    // steps have to shrink with the image's size.
    const coarse = [];
    for (let p = 1.5; p <= maxP; p += 0.25 * p / n) coarse.push(score(p));
    let i = coarse.length - 1;
    while (i >= 0 && coarse[i].cover < 0.75) i--;
    if (i < 0) return none;
    // Near the right size a whole range of sizes fits; take the middle of the best ones.
    let lo = i;
    while (lo > 0 && coarse[lo - 1].cover >= 0.75) lo--;
    const run = coarse.slice(lo, i + 1);
    const top = Math.max(...run.map(r => r.cover));
    const near = run.filter(r => r.cover >= top - 0.005);
    let pick = score((near[0].p + near[near.length - 1].p) / 2);
    if (pick.cover < top - 0.01) pick = near[Math.floor(near.length / 2)];
    return { cell: +pick.p.toFixed(3), ox: +(pick.ox % pick.p).toFixed(2), oy: +(pick.oy % pick.p).toFixed(2), cover: pick.cover };
  }

  // Where each art pixel's cell lies in the source along one axis.
  function axis(n, cell, off) {
    const start = off >= cell / 2 ? off - cell : off;  // a partial cell before the offset still counts
    const count = Math.max(1, Math.round((n - start) / cell));
    const inset = cell >= 3 ? cell * 0.22 : 0;          // ignore the blurry borders of each cell
    const lo = new Int32Array(count), hi = new Int32Array(count), mid = new Int32Array(count);
    const clamp = v => Math.min(n - 1, Math.max(0, v));
    for (let i = 0; i < count; i++) {
      const a = start + i * cell;
      lo[i] = clamp(Math.floor(a + inset));
      hi[i] = Math.max(lo[i] + 1, Math.min(n, Math.ceil(a + cell - inset)));
      mid[i] = clamp(Math.floor(a + cell / 2));
    }
    return { count, lo, hi, mid, start };
  }

  // One color per cell. mode: 'dominant' (the most common color, averaged
  // within its shade so compression noise doesn't leak in), 'center', 'mean'.
  // Shades closer than `tol` to a more common color take that color (what
  // JPEG and lossy GIFs smear around one flat color). Each color joins the
  // most common one near it, so the real colors win.
  function mergeSimilar(frames, tol) {
    const hist = new Map();
    for (const d of frames)
      for (let i = 0; i < d.length; i += 4) {
        if (d[i + 3] < 128) continue;
        const k = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        hist.set(k, (hist.get(k) || 0) + 1);
      }
    const keys = [...hist.keys()].sort((a, b) => hist.get(b) - hist.get(a));
    const centers = [], map = new Map(), t2 = tol * tol;
    for (const k of keys) {
      const r = k >> 16, g = (k >> 8) & 255, b = k & 255;
      let to = k;
      for (const c of centers) {
        const dr = r - (c >> 16), dg = g - ((c >> 8) & 255), db = b - (c & 255);
        if (dr * dr + dg * dg + db * db <= t2) { to = c; break; }
      }
      if (to === k) {
        if (centers.length >= 4096) break;  // a photo, not pixel art: leave the rest alone
        centers.push(k);
      }
      map.set(k, to);
    }
    for (const d of frames)
      for (let i = 0; i < d.length; i += 4) {
        if (d[i + 3] < 128) continue;
        const c = map.get((d[i] << 16) | (d[i + 1] << 8) | d[i + 2]);
        if (c === undefined) continue;
        d[i] = c >> 16; d[i + 1] = (c >> 8) & 255; d[i + 2] = c & 255;
      }
  }

  // tol > 0 merges similar shades (mergeSimilar()); colors > 0 then reduces
  // the whole animation to that many colors.
  function build(src, grid, mode, colors, tol = 0) {
    const ax = axis(src.w, grid.cell, grid.ox), ay = axis(src.h, grid.cell, grid.oy);
    const w = ax.count, h = ay.count, sw = src.w;
    const cap = (Math.ceil(grid.cell) + 2) ** 2;
    const bins = new Map(), cnt = new Int32Array(cap), rs = new Int32Array(cap), gs = new Int32Array(cap), bs = new Int32Array(cap);
    const frames = src.frames.map(fr => {
      const d = fr.rgba, out = new Uint8ClampedArray(w * h * 4);
      for (let j = 0, o = 0; j < h; j++)
        for (let i = 0; i < w; i++, o += 4) {
          if (mode === 'center') {
            const k = (ay.mid[j] * sw + ax.mid[i]) * 4;
            out[o] = d[k]; out[o + 1] = d[k + 1]; out[o + 2] = d[k + 2]; out[o + 3] = d[k + 3] < 128 ? 0 : 255;
            continue;
          }
          let r = 0, g = 0, b = 0, n = 0, clear = 0;
          bins.clear();
          let used = 0;
          for (let y = ay.lo[j]; y < ay.hi[j]; y++)
            for (let x = ax.lo[i]; x < ax.hi[i]; x++) {
              const k = (y * sw + x) * 4;
              if (d[k + 3] < 128) { clear++; continue; }
              n++;
              if (mode === 'mean') { r += d[k]; g += d[k + 1]; b += d[k + 2]; continue; }
              const key = ((d[k] >> 3) << 10) | ((d[k + 1] >> 3) << 5) | (d[k + 2] >> 3);
              let s = bins.get(key);
              if (s === undefined) { s = used++; bins.set(key, s); cnt[s] = rs[s] = gs[s] = bs[s] = 0; }
              cnt[s]++; rs[s] += d[k]; gs[s] += d[k + 1]; bs[s] += d[k + 2];
            }
          if (clear > n) { out[o + 3] = 0; continue; }
          if (mode !== 'mean') {
            let top = 0;
            for (let s = 1; s < used; s++) if (cnt[s] > cnt[top]) top = s;
            r = rs[top]; g = gs[top]; b = bs[top]; n = cnt[top];
          }
          out[o] = r / n; out[o + 1] = g / n; out[o + 2] = b / n; out[o + 3] = 255;
        }
      return out;
    });
    if (tol > 0) mergeSimilar(frames, tol);
    if (colors > 0) {
      const pal = Gif.palette(frames, colors);
      for (const d of frames)
        for (let i = 0; i < d.length; i += 4) {
          if (d[i + 3] < 128) continue;
          const c = pal.colors[pal.lookup.get((d[i] << 16) | (d[i + 1] << 8) | d[i + 2])];
          d[i] = c >> 16; d[i + 1] = (c >> 8) & 255; d[i + 2] = c & 255;
        }
    }
    return { w, h, startX: ax.start, startY: ay.start, frames: frames.map((rgba, f) => ({ rgba, delay: src.frames[f].delay })) };
  }

  // The window of art pixels starting at (x, y), each drawn scale x scale,
  // cut to outW x outH; anything outside the art (or transparent) is bg [r,g,b].
  // A scale under 1 shrinks the art: each screen pixel takes one art pixel,
  // or with smooth, the average of all it covers.
  function compose(art, rgba, x, y, scale, outW, outH, bg, smooth = false) {
    if (scale < 1 && smooth) return shrink(art, rgba, x, y, scale, outW, outH, bg);
    const out = new Uint8ClampedArray(outW * outH * 4);
    for (let Y = 0, o = 0; Y < outH; Y++) {
      const ay = Math.floor(Y / scale) + y;
      for (let X = 0; X < outW; X++, o += 4) {
        const axx = Math.floor(X / scale) + x;
        let k = -1;
        if (ay >= 0 && ay < art.h && axx >= 0 && axx < art.w) {
          k = (ay * art.w + axx) * 4;
          if (rgba[k + 3] < 128) k = -1;
        }
        if (k < 0) { out[o] = bg[0]; out[o + 1] = bg[1]; out[o + 2] = bg[2]; }
        else { out[o] = rgba[k]; out[o + 1] = rgba[k + 1]; out[o + 2] = rgba[k + 2]; }
        out[o + 3] = 255;
      }
    }
    return out;
  }

  // Which art pixels [lo, hi) each of n screen pixels covers at scale < 1.
  function spans(n, from, scale) {
    const lo = new Int32Array(n), hi = new Int32Array(n);
    for (let i = 0; i < n; i++) {
      lo[i] = Math.floor(from + i / scale + 1e-6);
      hi[i] = Math.max(lo[i] + 1, Math.floor(from + (i + 1) / scale + 1e-6));
    }
    return [lo, hi];
  }

  function shrink(art, rgba, x, y, scale, outW, outH, bg) {
    const out = new Uint8ClampedArray(outW * outH * 4);
    const [x0, x1] = spans(outW, x, scale), [y0, y1] = spans(outH, y, scale);
    for (let Y = 0, o = 0; Y < outH; Y++)
      for (let X = 0; X < outW; X++, o += 4) {
        let r = 0, g = 0, b = 0, n = 0;
        for (let ay = y0[Y]; ay < y1[Y]; ay++)
          for (let ax = x0[X]; ax < x1[X]; ax++, n++) {
            const k = (ay * art.w + ax) * 4;
            if (ay < 0 || ay >= art.h || ax < 0 || ax >= art.w || rgba[k + 3] < 128) { r += bg[0]; g += bg[1]; b += bg[2]; }
            else { r += rgba[k]; g += rgba[k + 1]; b += rgba[k + 2]; }
          }
        out[o] = r / n; out[o + 1] = g / n; out[o + 2] = b / n; out[o + 3] = 255;
      }
    return out;
  }

  return { detect, build, compose };
})();
