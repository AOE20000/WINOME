/* SPDX-License-Identifier: GPL-2.0-only
 * WINOME Official Website Application Controller
 * Coordinates Style A (Dark) <-> Style B (Light) Day/Night toggle,
 * seamless scroll-scrubbed Overview MG backdrop, live docs/*.md stream excerpts,
 * SPA hash routing (#/ vs #/docs/<path>#<anchor>), ZH/EN i18n, and GitHub Releases probe.
 */
(function () {
  "use strict";

  document.documentElement.classList.add("js");

  const EN = {
    page_title: "WINOME — Native GNOME Shell Desktop Environment for Windows",
    skip: "Skip to content",
    nav_overview: "Overview",
    nav_workspaces: "Multi-Monitor",
    nav_tour: "UI & Themes",
    nav_arch: "Safety & Plugins",
    nav_start: "Quick Start",
    nav_docs: "Docs",
    nav_download: "Download",
    hero_badge: "Free & Open Source · Windows 10 / 11 · Native · Zero System Injection",
    hero_title: 'Authentic <span class="accent-text">GNOME Shell</span>,<br>built natively for Windows.',
    hero_sub:
      "More than a visual skin — a standalone desktop environment bringing GNOME's Activities overview, multi-monitor workspaces, and top bar natively to Windows. Zero system file edits, zero process injection, and automatic taskbar restoration on exit or crash.",
    cta_download: "Download Portable",
    cta_docs: "Read Docs ↗",
    cta_explore: "Scroll to Explore ↓",
    m1_u: "Injection",
    m1_l: "No system file edits; cleanly restored on exit",
    m2_l: "Lightweight native core with minimal idle usage",
    m3_u: "Monitor Modes",
    m3_l: "Independent / Primary Only / Sync across displays",
    m4_l: "Watchdog restores the Windows taskbar instantly",
    ov_eyebrow: "01 · ACTIVITIES OVERVIEW",
    ov_title: "Tap Win, and your whole desktop breathes open",
    ov_lead:
      "Scroll smoothly to watch the background demonstrate WINOME's Activities overview: windows arrange cleanly, the workspace strip slides in, and the Dash rises on every monitor.",
    ov_f1_t: "Instant Opening, Zero Wallpaper Flash",
    ov_f1_d:
      "Captures all desktops and windows in one pass and reveals all displays together once ready — smooth, snappy, and free of wallpaper glitches.",
    ov_f2_t: "Type-to-Search & Start Menu Folder Sync",
    ov_f2_d:
      "Switch smoothly between desktop, window overview, and app grid at any moment. Start typing to search apps or open windows, with automatic Windows 11 Start Menu folder sync.",
    ov_md_fallback:
      "Primary and secondary displays share one unified overview engine — search, organize workspaces, and access a full Dash from any monitor.",
    jump_arch_ov: "Read Overview Architecture",
    ws_eyebrow: "02 · MULTI-MONITOR WORKSPACES",
    ws_title: "Three multi-monitor workspace modes, switched live",
    ws_lead:
      "Unlike Windows' global-only desktop switching, click any mode below to preview WINOME's three multi-monitor workspace strategies right in the background:",
    mode_ind: "Independent Per-Monitor",
    mode_pri: "Primary Only · M2 Promoted",
    mode_span: "Sync All Displays",
    mode_desc_ind:
      "<strong>Independent (Default):</strong> Each monitor keeps its own workspace list — switching Screen 1 never interrupts docs, terminals, or videos on Screen 2.",
    mode_desc_pri:
      "<strong>Primary Only (Display 2 Promoted):</strong> Choosing GNOME mode on Display 2 makes it the active Shell Primary (with optional Windows primary display sync) while Display 1 stays fixed.",
    mode_desc_span:
      "<strong>Sync All Displays:</strong> Every display switches workspaces together in lockstep with Windows virtual desktops while keeping a full Dash on each monitor.",
    ws_md_fallback:
      "Switch anytime between Independent per-monitor workspaces, Primary Only (with dynamic promotion on secondary displays), and synchronized switching.",
    jump_arch_ws: "Read Multi-Monitor Docs",
    tour_eyebrow: "03 · REAL CAPTURE & VISUAL FIDELITY",
    tour_title: "Faithful GNOME look, seamless Windows integration",
    tour_lead:
      "Beyond Overview — top bar, calendar & notification center, system tray & background apps, volume/brightness/Wi-Fi/Bluetooth/power Quick Settings, and live wallpaper support for Wallpaper Engine & Lively.",
    shot_alt: "WINOME live desktop screenshot showing top bar, Activities overview, workspaces, Dash and Quick Settings",
    shot_zoom: "Click to view full resolution ⤢",
    ui_md_fallback:
      "Directly follows upstream GNOME Shell stylesheets so spacing, rounded corners, and motion curves stay authentic.",
    jump_theme_doc: "Read Theme & UI Guide",
    arch_eyebrow: "04 · SAFETY & EXTENSIBILITY",
    arch_title: "Smooth like a system shell, safe like a regular app",
    arch_lead:
      "Never injects code into Windows system processes or patches system files. A separate watchdog monitors the shell and restores your native taskbar and windows automatically if anything goes wrong.",
    sf_1_t: "Zero Shortcut Conflicts & Fullscreen Auto-Hide",
    sf_1_d:
      "Tap Win for Overview or double-tap for App Grid while <code>Win+E</code>, <code>Win+V</code>, <code>Win+Shift+S</code>, and <code>Alt+Tab</code> keep working normally. Fullscreen games automatically hide only that display's bar.",
    sf_2_t: "Isolated Plugins & Quick Commands",
    sf_2_d:
      "Supports out-of-process extensions, the <code>Alt+F2</code> quick run dialog, plus a built-in developer console and CLI companion.",
    arch_md_fallback:
      "Runs cleanly as an independent process with an automatic recovery watchdog, safe mode startup, and one-click diagnostics export.",
    jump_arch_main: "Read Safety & Recovery Docs",
    jump_pitfalls: "Read Engineering Notes",
    dev_eyebrow: "05 · OPEN DOCUMENTATION",
    dev_title: "Live from the repository — always up to date",
    dev_lead:
      "No stale duplicate docs — every page in the Docs Center is loaded live from the repository's <code>docs/</code> folder so developers can dive straight in.",
    build_md_fallback:
      "With MSYS2 (UCRT64 + GTK4), run <code>./build.sh release</code> in the repo root to compile the ~4.8 MB release build and package the portable zip.",
    jump_build: "Read Build & Packaging Guide",
    jump_roadmap: "View Roadmap",
    dl_readme: "Documentation Index",
    dl_arch: "System Architecture",
    dl_theme: "Theme & UI Guide",
    dl_pitfalls: "Engineering Notes & Pitfalls",
    dl_road: "Roadmap (v0.1 → v1.0)",
    dl_log: "Engineering Dev Logs",
    start_eyebrow: "06 · QUICK START & SHORTCUTS",
    start_title: "Unzip to run, return to classic Windows anytime",
    start_lead:
      "No installer and no extra runtimes needed. Extract <code>WINOME-win64-portable.zip</code>, run <code>winome.exe</code>, or delete the folder to uninstall completely.",
    k1: "Toggle Activities Overview",
    k2: "Double-tap for App Grid",
    k3: "Open Windows Start Menu",
    k4: "Switch workspace on active display",
    k5: "Move window to adjacent workspace",
    k6: "Trigger Overview on any monitor",
    k6k: "↖ Top-Left Hot Corner",
    cmd_lang: "Command Line",
    copy: "Copy",
    copied: "Copied",
    code_c1: "# Immediately restore the native Windows taskbar (or double-click restore-taskbar.bat)",
    code_c2: "# Start in safe mode (skips external plugins and window theming)",
    q1: "Will WINOME break Windows or make my taskbar disappear?",
    a1: "No. WINOME never modifies system files or injects into system processes. It temporarily hides the native taskbar while running and restores it automatically on exit or crash, with <code>restore-taskbar.bat</code> included as a one-click fallback.",
    q2: "What happens when selecting Primary Only (GNOME mode) on Display 2?",
    a2: "Display 2 immediately becomes the active Shell Primary: workspace switches happen on Display 2 while Display 1 stays still, and both monitors keep a full Dash. You can also opt in to sync the Windows primary display in Settings.",
    q3: "Why does it ask for admin rights, and is there any telemetry?",
    a3: "Administrator permission is only used to hide the native taskbar and manage elevated windows across workspaces. WINOME is 100% offline with zero telemetry; settings and logs stay locally in <code>%LOCALAPPDATA%\\WINOME</code>.",
    cta_download2: "Download WINOME Portable",
    cta_docs2: "Open Full Docs Center →",
    cta_star: "GitHub Repository ↗",
    footer_disclaimer:
      "WINOME is an independent open-source project inspired by GNOME Shell and is not affiliated with or endorsed by the GNOME Foundation. Licensed under GPL-2.0-only.",
    footer_docs: "Docs Center",
    footer_arch: "Architecture",
    footer_rel: "Releases",
    footer_issues: "Issues",
    docs_home: "← Back to Home",
    docs_ssot: "Live from repo docs/",
    docs_gh: "View raw on GitHub ↗",
    docs_toc_title: "On This Page",
    drawer_expand: "Full View ⤢",
    rel_latest: "Latest release",
    rel_fallback: "Download WINOME-win64-portable.zip from GitHub Releases"
  };

  const ZH = {
    page_title: document.title,
    copy: "复制",
    copied: "已复制",
    mode_desc_ind:
      "<strong>内置独立切换（默认）：</strong>每个显示器拥有各自独立的工作区列表，切换屏幕 1 时完全不会打扰屏幕 2 的文档、终端或视频。",
    mode_desc_pri:
      "<strong>仅主屏 · 副屏2升主：</strong>在副屏 2 选择 GNOME 模式时，副屏 2 立即成为当前 Shell 主屏（可在设置中同步切换 Windows 主屏），屏幕 1 保持静止。",
    mode_desc_span:
      "<strong>全屏同步联动：</strong>所有显示器同步切换工作区，与 Windows 原生虚拟桌面保持一致，且每块屏幕底部都保留完整 Dash。",
    rel_latest: "最新版本",
    rel_fallback: "前往 GitHub Releases 下载 WINOME-win64-portable.zip"
  };

  document.querySelectorAll("[data-i18n]").forEach((el) => {
    const k = el.getAttribute("data-i18n");
    if (!(k in ZH)) ZH[k] = el.innerHTML;
  });
  document.querySelectorAll("[data-i18n-alt]").forEach((el) => {
    ZH[el.getAttribute("data-i18n-alt")] = el.getAttribute("alt");
  });

  // ========================================================================
  // 1. DAY / NIGHT THEME (Style A Dark <-> Style B Light)
  // ========================================================================
  let theme = document.documentElement.getAttribute("data-theme") || "dark";
  const themeToggle = document.getElementById("themeToggle");
  const themeMeta = document.querySelector('meta[name="theme-color"]');

  // ========================================================================
  // 2. LANGUAGE STATE
  // ========================================================================
  let lang = "zh";
  try {
    const saved = localStorage.getItem("winome_lang");
    if (saved === "zh" || saved === "en") lang = saved;
    else if (navigator.language && !navigator.language.toLowerCase().startsWith("zh")) lang = "en";
  } catch (_) {}

  const dict = () => (lang === "zh" ? ZH : EN);
  let activeMode = "independent";
  let savedHomeScrollY = 0;

  // ========================================================================
  // 3. INIT OVERVIEW BACKDROP MG & LIVE DOCS PORTAL
  // ========================================================================
  const mgCanvas = document.getElementById("overviewMgCanvas");
  const mgBackdrop =
    mgCanvas && window.WinomeOverviewBackdrop ? new window.WinomeOverviewBackdrop(mgCanvas) : null;

  const docsPortal = window.WinomeDocsPortal
    ? new window.WinomeDocsPortal({
        onNavigate: (docPath, anchorHash) => navigateToDoc(docPath, anchorHash)
      })
    : null;

  function applyTheme(nextTheme, persist) {
    theme = nextTheme === "light" ? "light" : "dark";
    document.documentElement.setAttribute("data-theme", theme);
    if (themeMeta) themeMeta.setAttribute("content", theme === "light" ? "#f5f3ec" : "#0b0e14");
    if (mgBackdrop) mgBackdrop.setTheme(theme);
    if (themeToggle) {
      themeToggle.setAttribute(
        "title",
        theme === "dark"
          ? lang === "zh"
            ? "切换至日间模式（瑞士纸墨长卷）"
            : "Switch to Light Mode (Swiss Paper)"
          : lang === "zh"
            ? "切换至夜间模式（Adwaita 雾幕暗境）"
            : "Switch to Dark Mode (Adwaita Mist)"
      );
    }
    if (persist) {
      try {
        localStorage.setItem("winome_theme", theme);
      } catch (_) {}
    }
  }

  if (themeToggle) {
    themeToggle.addEventListener("click", () => {
      applyTheme(theme === "dark" ? "light" : "dark", true);
    });
  }
  applyTheme(theme, false);

  // ========================================================================
  // 4. MODE PILLS (LIVE INTERACTION WITH OVERVIEW MG)
  // ========================================================================
  const modeDescEl = document.getElementById("modeDesc");
  function updateModeDescription() {
    if (!modeDescEl) return;
    const d = dict();
    const key =
      activeMode === "primary"
        ? "mode_desc_pri"
        : activeMode === "span"
          ? "mode_desc_span"
          : "mode_desc_ind";
    modeDescEl.innerHTML = d[key] || ZH[key];
  }

  document.querySelectorAll("#modePills .mode-pill").forEach((pill) => {
    pill.addEventListener("click", () => {
      activeMode = pill.dataset.mode || "independent";
      document.querySelectorAll("#modePills .mode-pill").forEach((p) => {
        p.classList.toggle("active", p === pill);
      });
      updateModeDescription();
      if (mgBackdrop) mgBackdrop.setMode(activeMode);
    });
  });

  // ========================================================================
  // 5. ROUTING (#/ HOME STREAM vs #/docs/<path>#<anchor>)
  // ========================================================================
  const viewHome = document.getElementById("viewHome");
  const viewDocs = document.getElementById("viewDocs");
  const mobileDrawer = document.getElementById("mobileDrawer");
  const mobileMenuBtn = document.getElementById("mobileMenuBtn");
  const topDots = Array.from(document.querySelectorAll("#topActivitiesDots .dot"));
  const navItems = Array.from(document.querySelectorAll(".nav-links [data-nav]"));

  function setActivitiesDot(idx) {
    topDots.forEach((d, i) => d.classList.toggle("active", i === idx));
  }

  function setActiveNav(name) {
    navItems.forEach((el) => el.classList.toggle("active", el.dataset.nav === name));
  }

  function closeMobileMenu() {
    if (mobileDrawer) mobileDrawer.classList.remove("open");
    if (mobileMenuBtn) mobileMenuBtn.setAttribute("aria-expanded", "false");
  }

  if (mobileMenuBtn && mobileDrawer) {
    mobileMenuBtn.addEventListener("click", () => {
      const open = mobileDrawer.classList.toggle("open");
      mobileMenuBtn.setAttribute("aria-expanded", String(open));
    });
  }

  function showHome(sectionId, restoreScroll) {
    const wasDocs = viewDocs && !viewDocs.hidden;
    if (viewHome) viewHome.hidden = false;
    if (viewDocs) viewDocs.hidden = true;
    document.body.classList.remove("mode-docs");
    if (mgBackdrop) {
      mgBackdrop.setPaused(false);
      mgBackdrop.resize();
    }

    if (sectionId) {
      setActiveNav(sectionId);
      const target = document.getElementById(sectionId);
      if (target) {
        setTimeout(() => {
          target.scrollIntoView({ behavior: "smooth", block: "start" });
          updateEditorialFocus();
        }, 30);
        return;
      }
    }

    if (restoreScroll && wasDocs) {
      window.scrollTo({ top: savedHomeScrollY, behavior: "instant" });
    } else if (!sectionId) {
      window.scrollTo({ top: 0, behavior: "smooth" });
    }
    setTimeout(() => {
      updateEditorialFocus();
      if (mgBackdrop) mgBackdrop.updateScrollTarget();
    }, 40);
  }

  function showDocs(docPath, anchorHash) {
    if (docsPortal && docsPortal.isDrawerOpen()) {
      docsPortal.closeDrawer();
    }
    if (viewHome && !viewHome.hidden) {
      savedHomeScrollY = window.scrollY;
    }
    if (viewHome) viewHome.hidden = true;
    if (viewDocs) viewDocs.hidden = false;
    document.body.classList.add("mode-docs");
    if (mgBackdrop) mgBackdrop.setPaused(true);
    setActivitiesDot(3);
    setActiveNav("docs");
    if (docsPortal) docsPortal.loadDoc(docPath || "architecture.md", anchorHash);
  }

  function navigateToDoc(docPath, anchorHash) {
    const clean = docPath || "architecture.md";
    const hash = anchorHash ? `#/docs/${clean}#${anchorHash}` : `#/docs/${clean}`;
    if (location.hash !== hash) {
      history.pushState(null, "", hash);
    }
    showDocs(clean, anchorHash);
  }

  function handleRoute() {
    const h = location.hash || "#/";
    if (h.startsWith("#/docs/")) {
      const rest = h.slice("#/docs/".length);
      const [docPath, anchor] = rest.split("#");
      showDocs(docPath || "architecture.md", anchor || "");
      return;
    }
    if (h === "#/docs") {
      showDocs("architecture.md", "");
      return;
    }
    const sectionId = h.replace(/^#\/?/, "");
    if (sectionId && document.getElementById(sectionId)) {
      showHome(sectionId, false);
    } else {
      showHome(null, false);
    }
  }

  window.addEventListener("hashchange", handleRoute);
  window.addEventListener("popstate", handleRoute);

  document.addEventListener("click", (e) => {
    const docBtn = e.target.closest("[data-open-doc]");
    if (docBtn) {
      e.preventDefault();
      closeMobileMenu();
      const targetDoc = docBtn.dataset.openDoc || "architecture.md";
      const targetAnchor = docBtn.dataset.docAnchor || "";
      const isStreamTrigger =
        viewHome &&
        !viewHome.hidden &&
        (docBtn.classList.contains("doc-jump-btn") || docBtn.classList.contains("doc-mini-link"));
      if (isStreamTrigger && docsPortal) {
        docsPortal.openDrawer(targetDoc, targetAnchor);
      } else {
        navigateToDoc(targetDoc, targetAnchor);
      }
      return;
    }

    const navA = e.target.closest(".nav-links a[href^='#'], .mobile-drawer a[href^='#'], .cta-row a[href^='#']");
    if (navA) {
      const targetId = navA.getAttribute("href").slice(1);
      if (!targetId) return;
      closeMobileMenu();
      if (viewHome && viewHome.hidden) {
        e.preventDefault();
        history.pushState(null, "", `#${targetId}`);
        showHome(targetId, false);
      }
    }
  });

  const docsBackHome = document.getElementById("docsBackHome");
  if (docsBackHome) {
    docsBackHome.addEventListener("click", () => {
      history.pushState(null, "", "#/");
      showHome(null, true);
    });
  }

  const brandHome = document.getElementById("brandHomeLink");
  if (brandHome) {
    brandHome.addEventListener("click", (e) => {
      e.preventDefault();
      history.pushState(null, "", "#/");
      showHome(null, false);
    });
  }

  // ========================================================================
  // 6. SMOOTH EDITORIAL SCROLL FOCUS & ACTIVITIES DOT SYNC
  // ========================================================================
  const chapters = Array.from(document.querySelectorAll(".stream-chapter"));

  function updateEditorialFocus() {
    if (viewHome && viewHome.hidden) return;
    const vpCenter = window.scrollY + window.innerHeight * 0.48;
    let closest = chapters[0];
    let bestDist = Infinity;

    chapters.forEach((sec) => {
      const rect = sec.getBoundingClientRect();
      const secCenter = window.scrollY + rect.top + rect.height * 0.5;
      const dist = Math.abs(vpCenter - secCenter);
      const norm = dist / Math.max(window.innerHeight * 0.72, 420);
      const block = sec.querySelector(".editorial-block");
      if (block && !block.classList.contains("always-visible")) {
        block.classList.toggle("in-focus", norm < 0.88);
      }
      if (dist < bestDist) {
        bestDist = dist;
        closest = sec;
      }
    });

    if (closest) {
      const dotIdx = Number(closest.dataset.dotIdx || 0);
      setActivitiesDot(dotIdx);
      if (closest.id && closest.id !== "top") {
        setActiveNav(closest.id);
      } else {
        setActiveNav("overview");
      }
    }
  }

  window.addEventListener("scroll", updateEditorialFocus, { passive: true });
  window.addEventListener("resize", updateEditorialFocus, { passive: true });
  updateEditorialFocus();

  // ========================================================================
  // 7. LANGUAGE SWITCHING (ZH <-> EN)
  // ========================================================================
  const langToggle = document.getElementById("langToggle");

  function applyLang(nextLang) {
    lang = nextLang === "en" ? "en" : "zh";
    try {
      localStorage.setItem("winome_lang", lang);
    } catch (_) {}

    const d = dict();
    document.documentElement.lang = lang === "zh" ? "zh-CN" : "en";
    document.title = d.page_title || ZH.page_title;

    document.querySelectorAll("[data-i18n]").forEach((el) => {
      const k = el.getAttribute("data-i18n");
      if (k in d) el.innerHTML = d[k];
    });
    document.querySelectorAll("[data-i18n-alt]").forEach((el) => {
      const k = el.getAttribute("data-i18n-alt");
      if (k in d) el.setAttribute("alt", d[k]);
    });

    if (langToggle) {
      langToggle.textContent = lang === "zh" ? "English" : "中文";
    }

    updateModeDescription();
    applyTheme(theme, false);
    if (mgBackdrop) mgBackdrop.setLang(lang);
    if (docsPortal) docsPortal.setLang(lang);
    renderReleaseMeta();
  }

  if (langToggle) {
    langToggle.addEventListener("click", () => {
      applyLang(lang === "zh" ? "en" : "zh");
    });
  }

  // ========================================================================
  // 8. COPY BUTTONS & SCREENSHOT LIGHTBOX
  // ========================================================================
  document.querySelectorAll("[data-copy-target]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      const target = document.getElementById(btn.dataset.copyTarget);
      if (!target) return;
      const lines = (target.innerText || target.textContent || "")
        .split("\n")
        .filter((l) => !l.trim().startsWith("#") && l.trim().length > 0);
      try {
        await navigator.clipboard.writeText(lines.join("\n"));
        btn.textContent = dict().copied;
        setTimeout(() => {
          btn.textContent = dict().copy;
        }, 1500);
      } catch (_) {}
    });
  });

  const lightbox = document.getElementById("lightbox");
  const shotTrigger = document.getElementById("shotTrigger");
  const lightboxClose = document.getElementById("lightboxClose");

  function closeLightbox() {
    if (lightbox) lightbox.hidden = true;
  }

  if (shotTrigger && lightbox) {
    const openLb = () => {
      lightbox.hidden = false;
    };
    shotTrigger.addEventListener("click", openLb);
    shotTrigger.addEventListener("keydown", (e) => {
      if (e.key === "Enter" || e.key === " ") {
        e.preventDefault();
        openLb();
      }
    });
    if (lightboxClose) lightboxClose.addEventListener("click", closeLightbox);
    lightbox.addEventListener("click", (e) => {
      if (e.target === lightbox) closeLightbox();
    });
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && !lightbox.hidden) closeLightbox();
    });
  }

  // ========================================================================
  // 9. GITHUB RELEASES LIVE PROBE
  // ========================================================================
  let latestRelease = null;
  const releaseMetaEl = document.getElementById("releaseMeta");
  const brandVersionTag = document.getElementById("brandVersionTag");

  function renderReleaseMeta() {
    if (!releaseMetaEl) return;
    const d = dict();
    if (!latestRelease) {
      releaseMetaEl.textContent = d.rel_fallback;
      return;
    }
    const sizeText = latestRelease.sizeMB ? ` · ${latestRelease.sizeMB} MB` : "";
    releaseMetaEl.textContent = `${d.rel_latest}: ${latestRelease.tag} · ${latestRelease.assetName}${sizeText}`;
  }

  fetch("https://api.github.com/repos/AOE20000/WINOME/releases/latest", {
    headers: { Accept: "application/vnd.github+json" }
  })
    .then((r) => (r.ok ? r.json() : null))
    .then((rel) => {
      if (!rel || !rel.tag_name) return;
      const zip = Array.isArray(rel.assets)
        ? rel.assets.find((a) => /\.zip$/i.test(a.name))
        : null;
      latestRelease = {
        tag: rel.tag_name,
        url: (zip && zip.browser_download_url) || rel.html_url,
        assetName: (zip && zip.name) || "WINOME-win64-portable.zip",
        sizeMB: zip && zip.size ? (zip.size / (1024 * 1024)).toFixed(1) : null
      };
      if (brandVersionTag) brandVersionTag.textContent = rel.tag_name;
      if (latestRelease.url) {
        document.querySelectorAll(".js-download-link").forEach((a) => {
          a.href = latestRelease.url;
        });
      }
      renderReleaseMeta();
    })
    .catch(() => {});

  // ========================================================================
  // 10. BOOTSTRAP
  // ========================================================================
  applyLang(lang);
  if (docsPortal) {
    docsPortal.hydrateStreamExcerpts(viewHome);
  }
  handleRoute();
})();
