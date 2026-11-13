// L / R scope, drawn like the plugin's ScopeView (plugin/PluginEditor.cpp)
// from a real turntable recording. assets/scope.bin is written by
//   tc_analyze test-timecode.wav --side B --nominal -18.1 --scope scope.bin 6.4 16.4
// (raw input normalised to the nominal level, every 2nd sample, pulled in to
// 1.4 x nominal) and played back in real time, looping.
(function () {
  const canvas = document.getElementById("scope");
  const statusEl = document.getElementById("scope-status");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  const kCapacity = 1024; // points kept by ScopeView
  const kEdge = 1.4;      // guide circle = 1.4 x nominal level

  // waveColour(): 16 steps interpolated between these anchors
  const anchors = [0x24507a, 0x2a7fa8, 0x2fb3b0, 0x3ddc97, 0x9be05a, 0xe8d44d, 0xff9a3d, 0xff4f3d];
  const steps = 16;
  const table = [];
  for (let i = 0; i < steps; i++) {
    const p = (i / (steps - 1)) * (anchors.length - 1);
    const j = Math.min(Math.floor(p), anchors.length - 2), f = p - j;
    const a = anchors[j], b = anchors[j + 1];
    const ch = (c, s) => (c >> s) & 255;
    const mix = (s) => Math.round(ch(a, s) + (ch(b, s) - ch(a, s)) * f);
    table.push(`rgb(${mix(16)},${mix(8)},${mix(0)})`);
  }
  const colourIndex = (a) => Math.max(0, Math.min(steps - 1, Math.floor(a * steps)));

  let w = 0, h = 0;
  function resize() {
    const dpr = window.devicePixelRatio || 1;
    const r = canvas.getBoundingClientRect();
    w = r.width; h = r.height;
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  function drawFrame(pts, end) {
    ctx.clearRect(0, 0, w, h);
    const cx = w / 2, cy = h / 2;
    const radius = 0.42 * Math.min(w, h);

    ctx.strokeStyle = "rgba(138,138,144,0.4)";
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.arc(cx, cy, radius, 0, 2 * Math.PI); ctx.stroke();
    ctx.lineWidth = 0.5;
    ctx.beginPath();
    ctx.moveTo(cx - radius, cy); ctx.lineTo(cx + radius, cy);
    ctx.moveTo(cx, cy - radius); ctx.lineTo(cx, cy + radius);
    ctx.stroke();

    ctx.fillStyle = "#8a8a90";
    ctx.font = '11px "JetBrains Mono", monospace';
    ctx.textBaseline = "top";
    ctx.fillText("L / R", 6, 6);

    if (!pts) return;
    // Values are already pulled in to the edge; 127 = edge.
    const n = pts.length / 2;
    const scale = radius / 127;
    const byColour = Array.from({ length: steps }, () => []);
    for (let k = 0; k < kCapacity; k++) {
      const i = (((end - kCapacity + k) % n) + n) % n;
      const x = pts[i * 2], y = pts[i * 2 + 1];
      const m = Math.sqrt(x * x + y * y) / 127;
      byColour[colourIndex(Math.min(m, 1))].push(cx + x * scale, cy - y * scale);
    }
    for (let c = 0; c < steps; c++) {
      const list = byColour[c];
      if (!list.length) continue;
      ctx.fillStyle = table[c];
      for (let k = 0; k < list.length; k += 2) ctx.fillRect(list[k] - 0.75, list[k + 1] - 0.75, 1.5, 1.5);
    }
  }

  function showStatus(view, statusOffset, statusCount, idx) {
    if (!statusEl || !statusCount) return;
    const o = statusOffset + (idx % statusCount) * 4;
    const speed = view.getInt16(o, true) / 1000, flags = view.getUint8(o + 2);
    const f = (b, s) => `${s} ${b ? "ok" : "--"}`;
    statusEl.textContent = `${f(flags & 1, "SIGNAL")} | ${f(flags & 2, "CODE")} | ${f(flags & 4, "ABS")} | speed ${speed.toFixed(3)}x`;
  }

  resize();
  window.addEventListener("resize", resize);
  drawFrame(null, 0);

  fetch("assets/scope.bin")
    .then((r) => (r.ok ? r.arrayBuffer() : Promise.reject(r.status)))
    .then((buf) => {
      const view = new DataView(buf);
      const magic = String.fromCharCode(...new Uint8Array(buf, 0, 4));
      if (magic !== "VTS1") return;
      const rate = view.getUint32(4, true), nPts = view.getUint32(8, true);
      const statusHz = view.getUint32(12, true), nStatus = view.getUint32(16, true);
      const pts = new Int8Array(buf, 20, nPts * 2);
      const statusOffset = 20 + nPts * 2;

      if (reduce) {
        const end = Math.round(1.5 * rate); // a still frame during normal play
        drawFrame(pts, end);
        showStatus(view, statusOffset, nStatus, Math.round(1.5 * statusHz));
        return;
      }
      let t0 = null;
      const frame = (now) => {
        if (t0 === null) t0 = now;
        const t = (now - t0) / 1000;
        drawFrame(pts, Math.floor(t * rate) % nPts);
        showStatus(view, statusOffset, nStatus, Math.floor(t * statusHz));
        requestAnimationFrame(frame);
      };
      requestAnimationFrame(frame);
    })
    .catch(() => {});
})();
