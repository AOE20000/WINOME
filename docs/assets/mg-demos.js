/* SPDX-License-Identifier: GPL-2.0-only
 * WINOME Seamless Scroll-Scrubbed Overview Motion Graphics Engine
 * Strictly dedicated to continuous GNOME Overview choreography across dual displays:
 * Desktop Idle -> Overview Open (Windows fan + Workspace strip + Dual Dash)
 * -> Workspace Glide (3 Secondary Monitor Modes) -> Cross-Monitor Window Drag
 * -> App Grid (Super+A) -> Overview Close back to Desktop.
 */
(function () {
  "use strict";

  const clamp = (v, a = 0, b = 1) => Math.max(a, Math.min(b, v));
  const lerp = (a, b, t) => a + (b - a) * t;
  const smoothstep = (e0, e1, x) => {
    const t = clamp((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
  };

  class WinomeOverviewBackdrop {
    constructor(canvasEl) {
      this.canvas = canvasEl;
      this.ctx = canvasEl ? canvasEl.getContext("2d") : null;
      this.hudPhaseEl = document.getElementById("hudPhase");
      this.hudFillEl = document.getElementById("hudFill");
      this.hudHintEl = document.getElementById("hudHint");

      this.theme = document.documentElement.getAttribute("data-theme") || "dark";
      this.lang = "zh";
      this.mode = "independent"; // independent | primary | span
      this.paused = false;
      this.reducedMotion = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

      this.cw = 1000;
      this.ch = 620;
      this.dpr = 1;
      this.targetProgress = 0;
      this.smoothProgress = 0;
      this.time = 0;
      this.rafId = 0;

      if (!this.canvas || !this.ctx) return;

      this.onResize = this.resize.bind(this);
      this.onScroll = this.updateScrollTarget.bind(this);
      this.onVisibility = () => {
        if (document.visibilityState === "hidden") this.stop();
        else if (!this.paused) this.start();
      };

      window.addEventListener("resize", this.onResize, { passive: true });
      window.addEventListener("scroll", this.onScroll, { passive: true });
      document.addEventListener("visibilitychange", this.onVisibility);

      this.resize();
      this.updateScrollTarget();
      this.smoothProgress = this.targetProgress;
      this.start();
    }

    setTheme(theme) {
      this.theme = theme === "light" ? "light" : "dark";
      if (this.reducedMotion) this.drawFrame();
    }

    setLang(lang) {
      this.lang = lang === "en" ? "en" : "zh";
      this.updateHud(this.smoothProgress);
      if (this.reducedMotion) this.drawFrame();
    }

    setMode(mode) {
      if (mode === "independent" || mode === "primary" || mode === "span") {
        this.mode = mode;
        this.updateHud(this.smoothProgress);
        if (this.reducedMotion) this.drawFrame();
      }
    }

    setPaused(flag) {
      this.paused = Boolean(flag);
      if (this.paused) this.stop();
      else this.start();
    }

    resize() {
      if (!this.canvas) return;
      const rect = this.canvas.getBoundingClientRect();
      this.cw = Math.max(320, rect.width || 1000);
      this.ch = Math.max(240, rect.height || 620);
      this.dpr = Math.min(window.devicePixelRatio || 1, 2);
      this.canvas.width = Math.round(this.cw * this.dpr);
      this.canvas.height = Math.round(this.ch * this.dpr);
      if (this.reducedMotion) this.drawFrame();
    }

    updateScrollTarget() {
      if (this.paused) return;
      const docEl = document.documentElement;
      const maxScroll = Math.max(1, docEl.scrollHeight - window.innerHeight);
      this.targetProgress = clamp(window.scrollY / maxScroll, 0, 1);
      if (this.reducedMotion) {
        this.smoothProgress = this.targetProgress;
        this.drawFrame();
      }
    }

    start() {
      if (this.rafId || this.paused) return;
      if (this.reducedMotion) {
        this.drawFrame();
        return;
      }
      const tick = () => {
        this.rafId = requestAnimationFrame(tick);
        this.time += 0.016;
        this.smoothProgress += (this.targetProgress - this.smoothProgress) * 0.11;
        this.drawFrame();
      };
      this.rafId = requestAnimationFrame(tick);
    }

    stop() {
      if (this.rafId) {
        cancelAnimationFrame(this.rafId);
        this.rafId = 0;
      }
    }

    getPalette() {
      if (this.theme === "light") {
        // Style B — Swiss Paper Editorial Light Palette
        return {
          bezel: "rgba(21, 27, 40, 0.14)",
          bezelFill: "#ffffff",
          shellBg: "#eaecef",
          wallA: "#dae4f6",
          wallB: "#e6ecf7",
          wallAccent: "rgba(29, 91, 224, 0.17)",
          topBar: "rgba(20, 24, 34, 0.90)",
          topBarText: "#f4f6fb",
          winBg: "#ffffff",
          winBar: "#edf0f5",
          winBorder: "rgba(21, 27, 40, 0.13)",
          winLine: "rgba(21, 27, 40, 0.14)",
          accent: "#1d5be0",
          accent2: "#0f8a5f",
          accent3: "#d97706",
          dashBg: "rgba(255, 255, 255, 0.88)",
          dashBorder: "rgba(21, 27, 40, 0.12)",
          wsThumbBg: "rgba(215, 226, 246, 0.82)",
          label: "#455066",
          shadow: "rgba(18, 28, 48, 0.12)"
        };
      }
      // Style A — Adwaita Atmospheric Dark Palette
      return {
        bezel: "rgba(255, 255, 255, 0.13)",
        bezelFill: "#111621",
        shellBg: "#141923",
        wallA: "#1b2d4e",
        wallB: "#1c2236",
        wallAccent: "rgba(74, 158, 255, 0.25)",
        topBar: "rgba(9, 12, 18, 0.90)",
        topBarText: "#eef3fc",
        winBg: "#1e2534",
        winBar: "#273044",
        winBorder: "rgba(255, 255, 255, 0.12)",
        winLine: "rgba(255, 255, 255, 0.14)",
        accent: "#4a9eff",
        accent2: "#57e389",
        accent3: "#f6b756",
        dashBg: "rgba(25, 31, 44, 0.84)",
        dashBorder: "rgba(255, 255, 255, 0.14)",
        wsThumbBg: "rgba(32, 47, 76, 0.86)",
        label: "#9aa8c2",
        shadow: "rgba(0, 0, 0, 0.46)"
      };
    }

    roundRect(x, y, w, h, r) {
      const ctx = this.ctx;
      const rr = Math.min(r, w / 2, h / 2);
      ctx.beginPath();
      ctx.moveTo(x + rr, y);
      ctx.arcTo(x + w, y, x + w, y + h, rr);
      ctx.arcTo(x + w, y + h, x, y + h, rr);
      ctx.arcTo(x, y + h, x, y, rr);
      ctx.arcTo(x, y, x + w, y, rr);
      ctx.closePath();
    }

    drawMiniWindow(x, y, w, h, p, accentColor, alpha = 1) {
      if (alpha <= 0.01) return;
      const ctx = this.ctx;
      ctx.save();
      ctx.globalAlpha *= alpha;
      ctx.shadowColor = p.shadow;
      ctx.shadowBlur = 15;
      ctx.shadowOffsetY = 6;
      this.roundRect(x, y, w, h, 8);
      ctx.fillStyle = p.winBg;
      ctx.fill();
      ctx.shadowBlur = 0;
      ctx.shadowOffsetY = 0;

      ctx.save();
      this.roundRect(x, y, w, h, 8);
      ctx.clip();

      ctx.fillStyle = p.winBar;
      ctx.fillRect(x, y, w, 15);
      ctx.fillStyle = accentColor;
      ctx.beginPath();
      ctx.arc(x + 9, y + 7.5, 2.5, 0, Math.PI * 2);
      ctx.fill();

      ctx.fillStyle = p.winLine;
      this.roundRect(x + 10, y + 23, w * 0.48, 4.5, 2);
      ctx.fill();
      this.roundRect(x + 10, y + 32, w * 0.72, 3.5, 2);
      ctx.fill();
      this.roundRect(x + 10, y + 40, w * 0.56, 3.5, 2);
      ctx.fill();

      if (h > 64) {
        ctx.fillStyle = accentColor;
        ctx.globalAlpha *= 0.22;
        this.roundRect(x + 10, y + 51, w - 20, Math.max(8, h - 61), 5);
        ctx.fill();
      }
      ctx.restore();

      this.roundRect(x, y, w, h, 8);
      ctx.strokeStyle = p.winBorder;
      ctx.lineWidth = 1;
      ctx.stroke();
      ctx.restore();
    }

    drawMonitor(cfg) {
      const ctx = this.ctx;
      const {
        x, y, w, h, p, label, primaryBadge,
        overviewAmt, wsShift, appGridAmt,
        windows, draggedOutAmt, draggedInAmt
      } = cfg;

      ctx.save();

      // Monitor outer bezel
      ctx.shadowColor = p.shadow;
      ctx.shadowBlur = 30;
      ctx.shadowOffsetY = 12;
      this.roundRect(x, y, w, h, 14);
      ctx.fillStyle = p.bezelFill;
      ctx.fill();
      ctx.shadowBlur = 0;
      ctx.shadowOffsetY = 0;

      ctx.strokeStyle = p.bezel;
      ctx.lineWidth = 1.25;
      ctx.stroke();

      // Monitor caption
      ctx.font = '600 11px "Segoe UI", "PingFang SC", sans-serif';
      ctx.fillStyle = p.label;
      ctx.fillText(label, x + 6, y - 10);

      if (primaryBadge > 0.05) {
        ctx.save();
        ctx.globalAlpha = primaryBadge;
        const badgeText = this.lang === "zh" ? "Shell 主屏" : "Shell Primary";
        const bw = this.lang === "zh" ? 72 : 84;
        const bh = 18;
        const bx = x + w - bw - 4;
        const by = y - 22;
        this.roundRect(bx, by, bw, bh, 9);
        ctx.fillStyle = p.accent;
        ctx.fill();
        ctx.fillStyle = "#ffffff";
        ctx.font = '600 10px "Segoe UI", "PingFang SC", sans-serif';
        ctx.textAlign = "center";
        ctx.fillText(badgeText, bx + bw / 2, by + 12.5);
        ctx.textAlign = "left";
        ctx.restore();
      }

      // Inner screen area
      const pad = 6;
      const sx = x + pad;
      const sy = y + pad;
      const sw = w - pad * 2;
      const sh = h - pad * 2;
      this.roundRect(sx, sy, sw, sh, 9);
      ctx.clip();

      ctx.fillStyle = p.shellBg;
      ctx.fillRect(sx, sy, sw, sh);

      // Top bar
      const tbH = 18;
      ctx.fillStyle = p.topBar;
      ctx.fillRect(sx, sy, sw, tbH);

      const pillX = sx + 10;
      const pillW = lerp(14, 21, overviewAmt);
      ctx.fillStyle = p.topBarText;
      this.roundRect(pillX, sy + 6.5, pillW, 5, 2.5);
      ctx.fill();
      ctx.globalAlpha = 0.36;
      ctx.beginPath();
      ctx.arc(pillX + pillW + 8, sy + 9, 2.3, 0, Math.PI * 2);
      ctx.fill();
      ctx.globalAlpha = 1;

      ctx.fillStyle = p.topBarText;
      ctx.font = '600 9.5px "Segoe UI", "PingFang SC", sans-serif';
      ctx.textAlign = "center";
      ctx.fillText(this.lang === "zh" ? "11月26日 10:30" : "Nov 26 10:30", sx + sw / 2, sy + 12.5);
      ctx.textAlign = "left";

      ctx.globalAlpha = 0.72;
      ctx.fillStyle = p.topBarText;
      this.roundRect(sx + sw - 34, sy + 6.5, 8, 5, 2);
      ctx.fill();
      this.roundRect(sx + sw - 22, sy + 6.5, 12, 5, 2);
      ctx.fill();
      ctx.globalAlpha = 1;

      // Workspace Strip
      const stripH = 38;
      const stripY = lerp(sy - stripH, sy + tbH + 8, overviewAmt);
      if (overviewAmt > 0.02) {
        ctx.save();
        ctx.globalAlpha = overviewAmt * (1 - appGridAmt * 0.55);
        const thumbW = 54;
        const thumbH = 30;
        const gap = 10;
        const totalW = thumbW * 2 + gap;
        const startX = sx + (sw - totalW) / 2;

        for (let i = 0; i < 2; i++) {
          const tx = startX + i * (thumbW + gap);
          this.roundRect(tx, stripY, thumbW, thumbH, 5);
          ctx.fillStyle = p.wsThumbBg;
          ctx.fill();

          ctx.fillStyle = i === 0 ? p.accent : p.accent2;
          ctx.globalAlpha = overviewAmt * 0.55 * (1 - appGridAmt * 0.55);
          this.roundRect(tx + 7, stripY + 7, 18, 13, 2);
          ctx.fill();
          this.roundRect(tx + 28, stripY + 10, 18, 12, 2);
          ctx.fill();
          ctx.globalAlpha = overviewAmt * (1 - appGridAmt * 0.55);
        }

        const activeTx = startX + wsShift * (thumbW + gap);
        this.roundRect(activeTx - 1.5, stripY - 1.5, thumbW + 3, thumbH + 3, 6.5);
        ctx.strokeStyle = p.accent;
        ctx.lineWidth = 2;
        ctx.stroke();
        ctx.restore();
      }

      // Main Workspace Cards (Scale & glide smoothly)
      const deskTop = sy + tbH;
      const deskH = sh - tbH;
      const wsScale = lerp(1.0, lerp(0.66, 0.34, appGridAmt), overviewAmt);
      const wsW = sw * wsScale;
      const wsH = deskH * wsScale;
      const wsCenterY = lerp(
        deskTop + deskH / 2,
        lerp(sy + tbH + 52 + wsH / 2, sy + tbH + 26 + wsH / 2, appGridAmt),
        overviewAmt
      );
      const wsGap = 18;

      for (let wsIdx = 0; wsIdx < 2; wsIdx++) {
        const offsetUnits = wsIdx - wsShift;
        const cx = sx + sw / 2 + offsetUnits * (wsW + wsGap);
        const wx = cx - wsW / 2;
        const wy = wsCenterY - wsH / 2;
        const radius = lerp(0, 11, overviewAmt);

        if (wx + wsW < sx - 10 || wx > sx + sw + 10) continue;

        ctx.save();
        this.roundRect(wx, wy, wsW, wsH, radius);
        ctx.clip();

        const g = ctx.createLinearGradient(wx, wy, wx + wsW, wy + wsH);
        g.addColorStop(0, wsIdx === 0 ? p.wallA : p.wallB);
        g.addColorStop(1, wsIdx === 0 ? p.wallB : p.wallA);
        ctx.fillStyle = g;
        ctx.fillRect(wx, wy, wsW, wsH);

        const rad = ctx.createRadialGradient(
          wx + wsW * (wsIdx === 0 ? 0.72 : 0.28),
          wy + wsH * 0.34,
          4,
          wx + wsW * 0.5,
          wy + wsH * 0.5,
          wsW * 0.65
        );
        rad.addColorStop(0, p.wallAccent);
        rad.addColorStop(1, "transparent");
        ctx.fillStyle = rad;
        ctx.fillRect(wx, wy, wsW, wsH);

        const wsWins = windows[wsIdx] || [];
        wsWins.forEach((win, idx) => {
          const isDraggedSource = wsIdx === 1 && idx === 0 && draggedOutAmt > 0.01;
          const curX = wx + lerp(win.dx, win.ox, overviewAmt) * wsW;
          const curY = wy + lerp(win.dy, win.oy, overviewAmt) * wsH;
          const curW = lerp(win.dw, win.ow, overviewAmt) * wsW;
          const curH = lerp(win.dh, win.oh, overviewAmt) * wsH;
          const alpha = isDraggedSource ? 1 - draggedOutAmt : 1;
          this.drawMiniWindow(curX, curY, curW, curH, p, win.color, alpha);
        });

        if (wsIdx === 1 && draggedInAmt > 0.01) {
          this.drawMiniWindow(
            wx + 0.52 * wsW,
            wy + 0.18 * wsH,
            0.40 * wsW,
            0.58 * wsH,
            p,
            p.accent,
            draggedInAmt
          );
        }

        ctx.restore();

        if (overviewAmt > 0.05) {
          this.roundRect(wx, wy, wsW, wsH, radius);
          ctx.strokeStyle = Math.abs(offsetUnits) < 0.5 ? p.accent : p.winBorder;
          ctx.globalAlpha = overviewAmt * (Math.abs(offsetUnits) < 0.5 ? 0.55 : 0.24);
          ctx.lineWidth = 1.25;
          ctx.stroke();
          ctx.globalAlpha = 1;
        }
      }

      // App Grid Matrix
      if (appGridAmt > 0.02) {
        ctx.save();
        ctx.globalAlpha = appGridAmt;
        const gridTop = sy + tbH + wsH + 40;
        const cols = 5;
        const rows = 2;
        const cell = Math.min(30, sw * 0.088);
        const gapX = Math.min(16, sw * 0.045);
        const gapY = 13;
        const gW = cols * cell + (cols - 1) * gapX;
        const gx0 = sx + (sw - gW) / 2;
        const colors = [
          p.accent, p.accent2, p.accent3, "#ec4899", "#8b5cf6",
          "#06b6d4", "#f97316", "#22c55e", "#3b82f6", "#eab308"
        ];

        for (let r = 0; r < rows; r++) {
          for (let c = 0; c < cols; c++) {
            const idx = r * cols + c;
            const ix = gx0 + c * (cell + gapX);
            const iy = gridTop + r * (cell + gapY) + (1 - appGridAmt) * 16;
            this.roundRect(ix, iy, cell, cell, 8);
            ctx.fillStyle = colors[idx % colors.length];
            ctx.globalAlpha = appGridAmt * 0.86;
            ctx.fill();
            ctx.fillStyle = "rgba(255,255,255,0.44)";
            this.roundRect(ix + cell * 0.27, iy + cell * 0.27, cell * 0.46, cell * 0.46, 4);
            ctx.fill();
          }
        }
        ctx.restore();
      }

      // Unified Dash Dock on BOTH monitors
      const dashH = 26;
      const dashW = Math.min(sw * 0.64, 186);
      const dashX = sx + (sw - dashW) / 2;
      const dashY = lerp(sy + sh + 12, sy + sh - dashH - 8, overviewAmt);

      if (overviewAmt > 0.02) {
        ctx.save();
        ctx.globalAlpha = overviewAmt;
        this.roundRect(dashX, dashY, dashW, dashH, 9);
        ctx.fillStyle = p.dashBg;
        ctx.fill();
        ctx.strokeStyle = p.dashBorder;
        ctx.lineWidth = 1;
        ctx.stroke();

        const icons = 6;
        const iSize = 14;
        const iGap = 11;
        const tot = icons * iSize + (icons - 1) * iGap;
        const ix0 = dashX + (dashW - tot) / 2;
        const dColors = [p.accent, p.accent2, p.accent3, "#a78bfa", "#f472b6", p.topBarText];
        for (let i = 0; i < icons; i++) {
          const ix = ix0 + i * (iSize + iGap);
          this.roundRect(ix, dashY + 6, iSize, iSize, 4);
          ctx.fillStyle = dColors[i];
          ctx.globalAlpha = overviewAmt * (i === icons - 1 ? 0.48 : 0.9);
          ctx.fill();
        }
        ctx.restore();
      }

      ctx.restore();
    }

    updateHud(t) {
      if (this.hudFillEl) this.hudFillEl.style.width = (t * 100).toFixed(1) + "%";
      if (!this.hudPhaseEl || !this.hudHintEl) return;

      const zh = this.lang === "zh";
      if (t < 0.13) {
        this.hudPhaseEl.textContent = zh ? "桌面待机" : "Desktop Idle";
        this.hudHintEl.textContent = zh ? "向下滚动展开概览" : "Scroll to open Overview";
      } else if (t < 0.30) {
        this.hudPhaseEl.textContent = zh ? "概览展开" : "Overview Open";
        this.hudHintEl.textContent = zh ? "窗口排布 · 双屏 Dash 升起" : "Windows fan · Dual Dash rises";
      } else if (t < 0.54) {
        const modeMap = zh
          ? { independent: "内置独立", primary: "副屏2升主", span: "全屏联动" }
          : { independent: "Independent", primary: "M2 Primary", span: "Span Sync" };
        this.hudPhaseEl.textContent = (zh ? "工作区 · " : "Workspaces · ") + modeMap[this.mode];
        this.hudHintEl.textContent = zh ? "可实时切换三种副屏模式" : "Interactive 3-mode switch";
      } else if (t < 0.80) {
        this.hudPhaseEl.textContent = zh ? "跨屏归类 & 应用网格" : "Cross-Drag & App Grid";
        this.hudHintEl.textContent = zh ? "窗口拖拽与 Super+A 抽屉" : "Window drag & Super+A";
      } else {
        this.hudPhaseEl.textContent = zh ? "概览收起" : "Back to Desktop";
        this.hudHintEl.textContent = zh ? "无缝回到聚焦工作区" : "Settling into active workspace";
      }
    }

    drawFrame() {
      const ctx = this.ctx;
      const t = this.smoothProgress;
      this.updateHud(t);

      ctx.save();
      ctx.scale(this.dpr, this.dpr);
      ctx.clearRect(0, 0, this.cw, this.ch);

      const p = this.getPalette();

      // Continuous scroll-scrubbed Overview choreography curves
      const enterOv = smoothstep(0.06, 0.22, t);
      const exitOv = smoothstep(0.84, 0.97, t);
      const overviewAmt = clamp(enterOv * (1 - exitOv), 0, 1);

      const wsPhase = smoothstep(0.26, 0.48, t);
      const dragPhase = smoothstep(0.50, 0.65, t);
      const appGridIn = smoothstep(0.62, 0.72, t);
      const appGridOut = smoothstep(0.77, 0.85, t);
      const appGridAmt = clamp(appGridIn * (1 - appGridOut), 0, 1);

      let wsShiftM1 = 0;
      let wsShiftM2 = 0;
      let badgeM1 = 0;
      let badgeM2 = 0;

      if (this.mode === "independent") {
        wsShiftM1 = wsPhase;
        wsShiftM2 = 0;
        badgeM1 = overviewAmt * 0.85;
      } else if (this.mode === "primary") {
        wsShiftM1 = 0;
        wsShiftM2 = wsPhase;
        badgeM2 = overviewAmt;
      } else {
        wsShiftM1 = wsPhase;
        wsShiftM2 = wsPhase;
        badgeM1 = overviewAmt * 0.85;
      }

      const breathY = this.reducedMotion ? 0 : Math.sin(this.time * 1.05) * 2.2;
      const gap = Math.min(28, this.cw * 0.03);
      const maxTotalW = Math.min(this.cw * 0.90, 860);
      const mW = (maxTotalW - gap) / 2;
      const mH = Math.min(mW * 0.64, this.ch * 0.56);
      const startX = (this.cw - (mW * 2 + gap)) / 2;
      const startY = (this.ch - mH) / 2 + breathY;

      const m1Windows = [
        [
          { dx: 0.06, dy: 0.08, dw: 0.58, dh: 0.74, ox: 0.06, oy: 0.14, ow: 0.42, oh: 0.68, color: p.accent },
          { dx: 0.36, dy: 0.18, dw: 0.56, dh: 0.72, ox: 0.52, oy: 0.18, ow: 0.42, oh: 0.64, color: p.accent2 }
        ],
        [
          { dx: 0.12, dy: 0.12, dw: 0.64, dh: 0.74, ox: 0.08, oy: 0.16, ow: 0.40, oh: 0.64, color: p.accent },
          { dx: 0.34, dy: 0.22, dw: 0.54, dh: 0.66, ox: 0.53, oy: 0.22, ow: 0.39, oh: 0.58, color: p.accent3 }
        ]
      ];

      const m2Windows = [
        [
          { dx: 0.08, dy: 0.10, dw: 0.80, dh: 0.78, ox: 0.10, oy: 0.16, ow: 0.44, oh: 0.66, color: p.accent2 },
          { dx: 0.46, dy: 0.24, dw: 0.46, dh: 0.62, ox: 0.58, oy: 0.20, ow: 0.34, oh: 0.58, color: p.accent3 }
        ],
        [
          { dx: 0.10, dy: 0.14, dw: 0.74, dh: 0.74, ox: 0.08, oy: 0.18, ow: 0.40, oh: 0.60, color: p.accent2 }
        ]
      ];

      const draggedOut = smoothstep(0.0, 0.25, dragPhase);
      const draggedIn = smoothstep(0.75, 1.0, dragPhase);

      this.drawMonitor({
        x: startX,
        y: startY,
        w: mW,
        h: mH,
        p,
        label: this.lang === "zh" ? "显示器 1 · 主屏" : "Display 1 · Primary",
        primaryBadge: badgeM1,
        overviewAmt,
        wsShift: wsShiftM1,
        appGridAmt,
        windows: m1Windows,
        draggedOutAmt: draggedOut,
        draggedInAmt: 0
      });

      this.drawMonitor({
        x: startX + mW + gap,
        y: startY,
        w: mW,
        h: mH,
        p,
        label: this.lang === "zh" ? "显示器 2 · 副屏" : "Display 2 · Secondary",
        primaryBadge: badgeM2,
        overviewAmt,
        wsShift: wsShiftM2,
        appGridAmt: appGridAmt * 0.25,
        windows: m2Windows,
        draggedOutAmt: 0,
        draggedInAmt: draggedIn
      });

      if (dragPhase > 0.05 && dragPhase < 0.95 && overviewAmt > 0.3) {
        const arc = Math.sin(dragPhase * Math.PI);
        const fx = lerp(startX + mW * 0.34, startX + mW + gap + mW * 0.62, dragPhase);
        const fy = lerp(startY + mH * 0.52, startY + mH * 0.50, dragPhase) - arc * 36;
        const fw = mW * 0.28 * (1 + arc * 0.08);
        const fh = mH * 0.32 * (1 + arc * 0.08);

        ctx.save();
        ctx.translate(fx, fy);
        ctx.rotate((0.5 - dragPhase) * -0.09 * arc);
        this.drawMiniWindow(-fw / 2, -fh / 2, fw, fh, p, p.accent, 0.96);
        ctx.restore();
      }

      ctx.restore();
    }
  }

  window.WinomeOverviewBackdrop = WinomeOverviewBackdrop;
})();
