/* SPDX-License-Identifier: GPL-2.0-only
 * WINOME Live Documentation Portal & Stream Excerpt Hydrator
 * Single source of truth: fetches live *.md directly from docs/ (zero duplication).
 */
(function () {
  "use strict";

  const REPO_BLOB = "https://github.com/AOE20000/WINOME/blob/main/docs/";

  const DOC_CATALOG = [
    {
      groupZh: "概览与快速上手",
      groupEn: "Overview & Getting Started",
      items: [
        { path: "README.md", zh: "文档中心导航", en: "Documentation Index", keywords: "index readme overview 模块" },
        { path: "build-and-env.md", zh: "开发环境与三方案构建", en: "Build & Environment", keywords: "msys2 ucrt64 meson ninja build.sh compile" }
      ]
    },
    {
      groupZh: "核心系统架构",
      groupEn: "Core Architecture",
      items: [
        { path: "architecture.md", zh: "系统架构与模块设计", en: "System Architecture", keywords: "overview watchdog ipc virtual desktop com systray multi-monitor" },
        { path: "dev-channel.md", zh: "开发者调试通道与 winome-ctl 指南", en: "Dev Channel & winome-ctl Guide", keywords: "dev channel winome-ctl ipc gtk.tree gtk.css gtk.node_at gtk.redraw inspect.panel test" },
        { path: "theme-and-ui.md", zh: "St 双管线与 UI 规范", en: "Theme Pipeline & UI", keywords: "st-compat css gtk4 clutter quick settings theme" },
        { path: "pitfalls.md", zh: "工程硬约束与踩坑汇总", en: "Constraints & Pitfalls", keywords: "win32 gtk4 lto fontconfig bugs constraints" }
      ]
    },
    {
      groupZh: "路线图与演进规划",
      groupEn: "Roadmap & Strategy",
      items: [
        { path: "roadmap.md", zh: "生产级完善路线图 (P0–P5)", en: "Production Roadmap", keywords: "p0 p1 p2 p3 p4 p5 milestone" },
        { path: "long-term-plan.md", zh: "长期演进蓝图", en: "Long-Term Blueprint", keywords: "future plan shell ecosystem" },
        { path: "file-manager-research.md", zh: "文件管理方案调研", en: "File Manager Research", keywords: "nautilus explorer files research" }
      ]
    },
    {
      groupZh: "阶段开发日志",
      groupEn: "Engineering Dev Logs",
      items: [
        { path: "dev-logs/2026-11-16.md", zh: "2026-11-16 · 多屏与自愈演进", en: "2026-11-16 · Multi-Monitor & Recovery", keywords: "log 2026-11-16" },
        { path: "dev-logs/2026-10-13.md", zh: "2026-10-13 · 主题与概览打磨", en: "2026-10-13 · Theme & Overview", keywords: "log 2026-10-13" },
        { path: "dev-logs/2026-10-11.md", zh: "2026-10-11 · 早期原型与架构选型", en: "2026-10-11 · Early Prototype", keywords: "log 2026-10-11" }
      ]
    }
  ];

  const FLAT_DOCS = DOC_CATALOG.flatMap((g) => g.items);

  function escapeHtml(s) {
    return s
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function slugify(text, used) {
    const base =
      text
        .toLowerCase()
        .replace(/<[^>]+>/g, "")
        .replace(/[^\p{L}\p{N}\s-]/gu, "")
        .trim()
        .replace(/\s+/g, "-") || "section";
    let slug = base;
    let i = 2;
    while (used.has(slug)) {
      slug = `${base}-${i++}`;
    }
    used.add(slug);
    return slug;
  }

  function resolveRelativePath(currentDoc, href) {
    if (!href || /^(https?:|mailto:|#)/i.test(href)) return null;
    const [rawPath, rawHash] = href.split("#");
    if (!/\.md$/i.test(rawPath)) return null;

    const baseParts = currentDoc.split("/").slice(0, -1);
    const relParts = rawPath.split("/");
    const stack = [...baseParts];
    for (const part of relParts) {
      if (!part || part === ".") continue;
      if (part === "..") {
        if (stack.length === 0) return null;
        stack.pop();
      } else {
        stack.push(part);
      }
    }
    const resolved = stack.join("/");
    if (FLAT_DOCS.some((d) => d.path === resolved)) {
      return { doc: resolved, hash: rawHash || "" };
    }
    return null;
  }

  function parseInline(raw, currentDoc) {
    const codeTokens = [];
    let s = raw.replace(/`([^`]+)`/g, (_, code) => {
      const id = `\u0000C${codeTokens.length}\u0000`;
      codeTokens.push(`<code>${escapeHtml(code)}</code>`);
      return id;
    });

    s = escapeHtml(s);

    s = s.replace(/!\[([^\]]*)\]\(([^)\s]+)\)/g, (_, alt, src) => {
      return `<img src="${escapeHtml(src)}" alt="${alt}" loading="lazy">`;
    });

    s = s.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_, label, href) => {
      const cleanHref = href.trim();
      const internal = resolveRelativePath(currentDoc, cleanHref);
      if (internal) {
        const hashAttr = internal.hash ? ` data-doc-hash="${escapeHtml(internal.hash)}"` : "";
        return `<a href="#/docs/${internal.doc}" data-doc-link="${internal.doc}"${hashAttr}>${label}</a>`;
      }
      if (/^\.\.\//.test(cleanHref)) {
        const gh = "https://github.com/AOE20000/WINOME/blob/main/" + cleanHref.replace(/^\.\.\//, "");
        return `<a href="${gh}" target="_blank" rel="noopener">${label}</a>`;
      }
      if (/^https?:/i.test(cleanHref)) {
        return `<a href="${cleanHref}" target="_blank" rel="noopener">${label}</a>`;
      }
      return `<a href="${cleanHref}">${label}</a>`;
    });

    s = s.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
    s = s.replace(/~~([^~]+)~~/g, "<del>$1</del>");
    s = s.replace(/(^|[^*])\*([^*\n]+)\*/g, "$1<em>$2</em>");

    s = s.replace(/\u0000C(\d+)\u0000/g, (_, idx) => codeTokens[Number(idx)] || "");
    return s;
  }

  function renderMarkdown(mdText, currentDoc, copyLabel) {
    const cleaned = mdText.replace(/<!--[\s\S]*?-->/g, "");
    const lines = cleaned.replace(/\r\n/g, "\n").split("\n");
    const out = [];
    const toc = [];
    const usedSlugs = new Set();

    function renderMermaidFlow(codeText) {
      const nodes = [];
      const seen = new Set();
      const re = /\b([A-Za-z0-9_]+)\s*\["([^"]+)"\]/g;
      let m;
      while ((m = re.exec(codeText)) !== null) {
        if (!seen.has(m[1])) {
          seen.add(m[1]);
          nodes.push(m[2].trim());
        }
      }
      if (!nodes.length) return null;
      const pills = nodes
        .map((label, idx) => {
          const arrow = idx < nodes.length - 1 ? `<span class="md-flow-arrow" aria-hidden="true">→</span>` : "";
          return `<span class="md-flow-node">${escapeHtml(label)}</span>${arrow}`;
        })
        .join("");
      return `<div class="md-flow-strip" role="list" aria-label="Milestone flow">${pills}</div>`;
    }

    function consumeFencedBlock(startIdx) {
      const m = lines[startIdx].match(/^(\s*)```([\w+-]*)\s*$/);
      if (!m) return null;
      const indentLen = m[1].length;
      const lang = m[2] || "";
      const buf = [];
      let j = startIdx + 1;
      while (j < lines.length && !/^\s*```\s*$/.test(lines[j])) {
        const rawLine = lines[j];
        const stripped =
          indentLen > 0 && rawLine.startsWith(" ".repeat(indentLen))
            ? rawLine.slice(indentLen)
            : rawLine;
        buf.push(stripped);
        j++;
      }
      if (j < lines.length) j++;
      const codeText = buf.join("\n");
      if (lang.toLowerCase() === "mermaid") {
        const flowHtml = renderMermaidFlow(codeText);
        if (flowHtml) return { html: flowHtml, nextIdx: j };
      }
      const langBadge = lang ? `<span class="md-lang">${escapeHtml(lang)}</span>` : "";
      const html = `<div class="md-pre-wrap">${langBadge}<button type="button" class="copy-btn md-copy-btn" data-md-copy="${encodeURIComponent(codeText)}">${copyLabel}</button><pre><code>${escapeHtml(codeText)}</code></pre></div>`;
      return { html, nextIdx: j };
    }

    let i = 0;
    while (i < lines.length) {
      const line = lines[i];

      if (/^\s*$/.test(line)) {
        i++;
        continue;
      }

      const fenced = consumeFencedBlock(i);
      if (fenced) {
        out.push(fenced.html);
        i = fenced.nextIdx;
        continue;
      }

      if (/^\s*(?:---+|___+|\*\*\*+)\s*$/.test(line)) {
        out.push("<hr>");
        i++;
        continue;
      }

      const h = line.match(/^(#{1,4})\s+(.+?)\s*#*\s*$/);
      if (h) {
        const level = h[1].length;
        const inner = parseInline(h[2], currentDoc);
        const plain = h[2]
          .replace(/`([^`]+)`/g, "$1")
          .replace(/\[([^\]]+)\]\([^)]+\)/g, "$1")
          .replace(/[*_~]/g, "");
        const slug = slugify(plain, usedSlugs);
        if (level === 2 || level === 3) {
          toc.push({ level, id: slug, text: plain });
        }
        out.push(`<h${level} id="${slug}">${inner}</h${level}>`);
        i++;
        continue;
      }

      if (/^\s*>\s?/.test(line)) {
        const bq = [];
        while (i < lines.length && /^\s*>\s?/.test(lines[i])) {
          bq.push(lines[i].replace(/^\s*>\s?/, ""));
          i++;
        }
        const first = bq[0] ? bq[0].trim() : "";
        const alertMatch = first.match(/^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]$/i);
        if (alertMatch) {
          const kind = alertMatch[1].toLowerCase();
          const titleMap = {
            note: "说明",
            tip: "提示",
            important: "重要",
            warning: "注意",
            caution: "警告"
          };
          const bodyText = bq.slice(1).join(" ").trim();
          out.push(
            `<div class="md-callout md-callout-${kind}"><div class="md-callout-title">${titleMap[kind] || kind}</div><p>${parseInline(bodyText, currentDoc)}</p></div>`
          );
        } else {
          out.push(`<blockquote><p>${parseInline(bq.join(" "), currentDoc)}</p></blockquote>`);
        }
        continue;
      }

      if (
        line.includes("|") &&
        i + 1 < lines.length &&
        /^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)+\|?\s*$/.test(lines[i + 1])
      ) {
        const splitRow = (row) =>
          row
            .trim()
            .replace(/^\|/, "")
            .replace(/\|$/, "")
            .split("|")
            .map((c) => c.trim());

        const headers = splitRow(line);
        i += 2;
        const rows = [];
        while (i < lines.length && lines[i].includes("|") && !/^\s*$/.test(lines[i])) {
          rows.push(splitRow(lines[i]));
          i++;
        }
        const thead = `<thead><tr>${headers.map((c) => `<th>${parseInline(c, currentDoc)}</th>`).join("")}</tr></thead>`;
        const tbody = `<tbody>${rows
          .map((r) => `<tr>${headers.map((_, ci) => `<td>${parseInline(r[ci] || "", currentDoc)}</td>`).join("")}</tr>`)
          .join("")}</tbody>`;
        out.push(`<div class="md-table-wrap"><table>${thead}${tbody}</table></div>`);
        continue;
      }

      if (/^\s*(?:[-*+]|\d+\.)\s+/.test(line)) {
        const ordered = /^\s*\d+\.\s+/.test(line);
        const tag = ordered ? "ol" : "ul";
        const items = [];
        while (i < lines.length && /^\s*(?:[-*+]|\d+\.)\s+/.test(lines[i])) {
          let content = lines[i].replace(/^\s*(?:[-*+]|\d+\.)\s+/, "");
          const extraBlocks = [];
          i++;
          while (i < lines.length) {
            if (/^\s*$/.test(lines[i])) {
              let peek = i + 1;
              while (peek < lines.length && /^\s*$/.test(lines[peek])) peek++;
              if (peek < lines.length && /^\s{2,}```/.test(lines[peek])) {
                i = peek;
                continue;
              }
              break;
            }
            if (/^\s{2,}```[\w+-]*\s*$/.test(lines[i])) {
              const nested = consumeFencedBlock(i);
              if (nested) {
                extraBlocks.push(nested.html);
                i = nested.nextIdx;
                continue;
              }
            }
            if (
              !/^\s*(?:[-*+]|\d+\.)\s+/.test(lines[i]) &&
              !/^#{1,4}\s+/.test(lines[i]) &&
              !/^\s*```/.test(lines[i]) &&
              /^\s{2,}/.test(lines[i])
            ) {
              content += " " + lines[i].trim();
              i++;
              continue;
            }
            break;
          }
          const task = content.match(/^\[([ xX])\]\s+([\s\S]+)$/);
          const suffix = extraBlocks.join("");
          if (task) {
            const done = task[1].toLowerCase() === "x";
            items.push(
              `<li class="task-item"><span class="task-box ${done ? "checked" : ""}" aria-hidden="true">${done ? "✓" : ""}</span><span>${parseInline(task[2], currentDoc)}${suffix}</span></li>`
            );
          } else {
            items.push(`<li>${parseInline(content, currentDoc)}${suffix}</li>`);
          }
        }
        out.push(`<${tag}>${items.join("")}</${tag}>`);
        continue;
      }

      const pLines = [];
      while (
        i < lines.length &&
        !/^\s*$/.test(lines[i]) &&
        !/^#{1,4}\s+/.test(lines[i]) &&
        !/^```/.test(lines[i]) &&
        !/^\s*>\s?/.test(lines[i]) &&
        !/^\s*(?:[-*+]|\d+\.)\s+/.test(lines[i]) &&
        !/^\s*(?:---+|___+|\*\*\*+)\s*$/.test(lines[i])
      ) {
        pLines.push(lines[i].trim());
        i++;
      }
      if (pLines.length) {
        out.push(`<p>${parseInline(pLines.join(" "), currentDoc)}</p>`);
      }
    }

    return { html: out.join("\n"), toc };
  }

  /**
   * Extract a clean, concise live Markdown slice for a specific heading from raw .md text.
   * Returns { html, anchor, headingText } so the homepage stream stays 100% live from docs/*.md.
   */
  function extractSectionExcerpt(mdText, docPath, matchStr, maxItems) {
    const lines = mdText.replace(/\r\n/g, "\n").split("\n");
    const usedSlugs = new Set();
    let targetStart = -1;
    let targetLevel = 2;
    let targetAnchor = "";
    let targetHeading = "";

    for (let i = 0; i < lines.length; i++) {
      const m = lines[i].match(/^(#{1,4})\s+(.+?)\s*#*\s*$/);
      if (!m) continue;
      const plain = m[2]
        .replace(/`([^`]+)`/g, "$1")
        .replace(/\[([^\]]+)\]\([^)]+\)/g, "$1")
        .replace(/[*_~]/g, "");
      const slug = slugify(plain, usedSlugs);
      if (targetStart < 0 && plain.toLowerCase().includes(matchStr.toLowerCase())) {
        targetStart = i + 1;
        targetLevel = m[1].length;
        targetAnchor = slug;
        targetHeading = plain;
      }
    }

    if (targetStart < 0) return null;

    // Collect lines until the next heading of same or higher level
    const sectionLines = [];
    for (let i = targetStart; i < lines.length; i++) {
      const hm = lines[i].match(/^(#{1,4})\s+/);
      if (hm && hm[1].length <= targetLevel) break;
      sectionLines.push(lines[i]);
    }

    const sectionRaw = sectionLines.join("\n");
    const explicit = sectionRaw.match(/<!--\s*excerpt:start\s*-->([\s\S]*?)<!--\s*excerpt:end\s*-->/i);
    if (explicit && explicit[1].trim()) {
      const rendered = renderMarkdown(explicit[1].trim(), docPath, "复制");
      return {
        html: rendered.html,
        anchor: targetAnchor,
        headingText: targetHeading
      };
    }

    // Fallback when no explicit excerpt markers exist: collect concise intro lines
    const introLines = [];
    let inFence = false;
    for (let i = 0; i < sectionLines.length; i++) {
      const line = sectionLines[i];
      if (/^\s*```/.test(line)) {
        inFence = !inFence;
        continue;
      }
      if (inFence) continue;
      const hm = line.match(/^(#{1,4})\s+/);
      if (hm && hm[1].length > targetLevel) {
        if (introLines.filter((l) => l.trim()).length >= 2) break;
        continue;
      }
      introLines.push(line);
    }

    const trimmed = [];
    let listCount = 0;
    let tableRows = 0;
    let paraCount = 0;
    const cap = Number(maxItems) || 3;

    for (let i = 0; i < introLines.length; i++) {
      const l = introLines[i];
      if (/^\s*$/.test(l)) {
        if (trimmed.length && trimmed[trimmed.length - 1] !== "") trimmed.push("");
        continue;
      }
      if (/^\s*(?:[-*+]|\d+\.)\s+/.test(l)) {
        listCount++;
        if (listCount <= cap) trimmed.push(l);
        continue;
      }
      if (l.includes("|")) {
        tableRows++;
        if (tableRows <= cap + 2) trimmed.push(l);
        continue;
      }
      if (listCount === 0 && tableRows === 0) {
        paraCount++;
        if (paraCount <= 2) trimmed.push(l);
      }
    }

    const rendered = renderMarkdown(trimmed.join("\n"), docPath, "复制");
    return {
      html: rendered.html,
      anchor: targetAnchor,
      headingText: targetHeading
    };
  }

  class WinomeDocsPortal {
    constructor(opts) {
      this.navEl = document.getElementById("docsNavList");
      this.searchEl = document.getElementById("docsSearchInput");
      this.bodyEl = document.getElementById("docsArticleBody");
      this.tocEl = document.getElementById("docsTocList");
      this.crumbEl = document.getElementById("docsBreadcrumb");
      this.ghLinkEl = document.getElementById("docsGhLink");
      this.pagerEl = document.getElementById("docsPager");

      this.drawerBackdropEl = document.getElementById("docDrawerBackdrop");
      this.drawerEl = document.getElementById("docDrawer");
      this.drawerTitleEl = document.getElementById("docDrawerTitle");
      this.drawerBodyEl = document.getElementById("docDrawerBody");
      this.drawerGhEl = document.getElementById("docDrawerGh");
      this.drawerExpandEl = document.getElementById("docDrawerExpand");
      this.drawerCloseEl = document.getElementById("docDrawerClose");
      this.drawerDoc = "architecture.md";
      this.drawerAnchor = "";

      this.lang = "zh";
      this.currentDoc = "architecture.md";
      this.cache = new Map();
      this.inFlight = new Map();
      this.tocObserver = null;
      this.onNavigate = opts && opts.onNavigate ? opts.onNavigate : null;

      if (this.searchEl) {
        this.searchEl.addEventListener("input", () => this.renderNav(this.searchEl.value));
      }
      if (this.drawerCloseEl) {
        this.drawerCloseEl.addEventListener("click", () => this.closeDrawer());
      }
      if (this.drawerBackdropEl) {
        this.drawerBackdropEl.addEventListener("click", () => this.closeDrawer());
      }
      if (this.drawerExpandEl) {
        this.drawerExpandEl.addEventListener("click", () => {
          const doc = this.drawerDoc;
          const anchor = this.drawerAnchor;
          this.closeDrawer();
          if (this.onNavigate) this.onNavigate(doc, anchor);
        });
      }
      document.addEventListener("keydown", (e) => {
        if (e.key === "Escape" && this.isDrawerOpen()) {
          this.closeDrawer();
        }
      });
      this.renderNav("");
    }

    isDrawerOpen() {
      return Boolean(this.drawerEl && this.drawerEl.classList.contains("open"));
    }

    closeDrawer() {
      if (this.drawerEl) {
        this.drawerEl.classList.remove("open");
        this.drawerEl.setAttribute("aria-hidden", "true");
      }
      if (this.drawerBackdropEl) {
        this.drawerBackdropEl.classList.remove("open");
      }
    }

    async openDrawer(docPath, anchorHash) {
      if (!this.drawerEl || !this.drawerBodyEl) {
        if (this.onNavigate) this.onNavigate(docPath, anchorHash);
        return;
      }
      const valid = FLAT_DOCS.find((d) => d.path === docPath);
      const target = valid ? valid.path : "architecture.md";
      this.drawerDoc = target;
      this.drawerAnchor = anchorHash || "";

      if (this.drawerTitleEl) this.drawerTitleEl.textContent = `docs/${target}`;
      if (this.drawerGhEl) this.drawerGhEl.href = REPO_BLOB + target;

      this.drawerEl.classList.add("open");
      this.drawerEl.setAttribute("aria-hidden", "false");
      if (this.drawerBackdropEl) this.drawerBackdropEl.classList.add("open");

      if (!this.cache.has(target)) {
        this.drawerBodyEl.innerHTML = `<p style="color:var(--text-3)">${this.lang === "zh" ? "正在实时加载 docs/" + target + " ..." : "Loading live docs/" + target + " ..."}</p>`;
      }

      try {
        const md = await this.fetchRawDoc(target);
        this.renderDrawerContent(target, md, this.drawerAnchor);
      } catch (err) {
        const zh = this.lang === "zh";
        this.drawerBodyEl.innerHTML = `
          <div class="docs-fallback">
            <h3>${zh ? "无法加载文档" : "Unable to load document"}</h3>
            <p>${escapeHtml(String(err))}</p>
            <p style="margin-top:10px"><a href="${REPO_BLOB + target}" target="_blank" rel="noopener">${zh ? "在 GitHub 上直接查看 →" : "View on GitHub →"}</a></p>
          </div>
        `;
      }
    }

    renderDrawerContent(docPath, mdText, anchorHash) {
      if (!this.drawerBodyEl) return;
      const zh = this.lang === "zh";
      const copyLbl = zh ? "复制" : "Copy";
      const copiedLbl = zh ? "已复制" : "Copied";
      const { html } = renderMarkdown(mdText, docPath, copyLbl);
      // Avoid duplicate IDs with full #viewDocs by prefixing drawer heading IDs
      const prefixedHtml = html.replace(/\bid="([^"]+)"/g, 'id="drawer-$1"');
      this.drawerBodyEl.innerHTML = prefixedHtml;

      this.drawerBodyEl.querySelectorAll("[data-doc-link]").forEach((a) => {
        a.addEventListener("click", (e) => {
          e.preventDefault();
          this.openDrawer(a.dataset.docLink, a.dataset.docHash || "");
        });
      });

      this.drawerBodyEl.querySelectorAll(".md-copy-btn").forEach((btn) => {
        btn.addEventListener("click", async () => {
          try {
            await navigator.clipboard.writeText(decodeURIComponent(btn.dataset.mdCopy || ""));
            btn.textContent = copiedLbl;
            setTimeout(() => {
              btn.textContent = copyLbl;
            }, 1400);
          } catch (_) {}
        });
      });

      if (anchorHash) {
        const el = this.drawerBodyEl.querySelector(`#drawer-${CSS.escape(anchorHash)}`);
        if (el) {
          setTimeout(() => {
            el.scrollIntoView({ behavior: "smooth", block: "start" });
          }, 80);
          return;
        }
      }
      this.drawerBodyEl.scrollTop = 0;
    }

    async fetchRawDoc(docPath) {
      if (this.cache.has(docPath)) return this.cache.get(docPath);
      if (this.inFlight.has(docPath)) return this.inFlight.get(docPath);

      const p = fetch(`./${docPath}`, { cache: "no-cache" })
        .then((res) => {
          if (!res.ok) throw new Error(`HTTP ${res.status}`);
          return res.text();
        })
        .then((md) => {
          this.cache.set(docPath, md);
          this.inFlight.delete(docPath);
          return md;
        })
        .catch((err) => {
          this.inFlight.delete(docPath);
          throw err;
        });

      this.inFlight.set(docPath, p);
      return p;
    }

    async hydrateStreamExcerpts(rootEl) {
      const nodes = Array.from((rootEl || document).querySelectorAll(".md-live-excerpt[data-md-doc]"));
      if (!nodes.length) return;

      const uniqueDocs = [...new Set(nodes.map((n) => n.dataset.mdDoc).filter(Boolean))];
      await Promise.allSettled(uniqueDocs.map((doc) => this.fetchRawDoc(doc)));

      nodes.forEach((node) => {
        const docPath = node.dataset.mdDoc;
        const matchStr = node.dataset.mdMatch || "";
        const maxItems = node.dataset.mdMax || "3";
        const bodyEl = node.querySelector(".md-excerpt-body");
        const jumpBtn = node.querySelector("[data-open-doc]");

        if (!this.cache.has(docPath) || !bodyEl) return;

        const slice = extractSectionExcerpt(this.cache.get(docPath), docPath, matchStr, maxItems);
        if (!slice || !slice.html) return;

        bodyEl.innerHTML = slice.html;
        bodyEl.querySelectorAll("[data-doc-link]").forEach((a) => {
          a.addEventListener("click", (e) => {
            e.preventDefault();
            const nextDoc = a.dataset.docLink;
            const nextHash = a.dataset.docHash || "";
            if (this.onNavigate) this.onNavigate(nextDoc, nextHash);
          });
        });

        if (jumpBtn && slice.anchor) {
          jumpBtn.dataset.docAnchor = slice.anchor;
        }
      });
    }

    setLang(lang) {
      this.lang = lang === "en" ? "en" : "zh";
      if (this.searchEl) {
        this.searchEl.placeholder =
          this.lang === "zh" ? "筛选文档标题或关键词..." : "Filter docs by title or keyword...";
      }
      this.renderNav(this.searchEl ? this.searchEl.value : "");
      if (this.cache.has(this.currentDoc)) {
        this.renderDocContent(this.currentDoc, this.cache.get(this.currentDoc));
      }
    }

    renderNav(query) {
      if (!this.navEl) return;
      const q = (query || "").trim().toLowerCase();
      const zh = this.lang === "zh";

      const html = DOC_CATALOG.map((grp) => {
        const matched = grp.items.filter((it) => {
          if (!q) return true;
          return (
            it.zh.toLowerCase().includes(q) ||
            it.en.toLowerCase().includes(q) ||
            it.path.toLowerCase().includes(q) ||
            it.keywords.toLowerCase().includes(q)
          );
        });
        if (!matched.length) return "";
        return `
          <div class="docs-group">
            <div class="docs-group-title">${zh ? grp.groupZh : grp.groupEn}</div>
            ${matched
              .map(
                (it) => `
                <button type="button" class="docs-nav-item ${it.path === this.currentDoc ? "active" : ""}" data-doc-path="${it.path}">
                  <span>${zh ? it.zh : it.en}</span>
                  <small>${it.path}</small>
                </button>
              `
              )
              .join("")}
          </div>
        `;
      }).join("");

      this.navEl.innerHTML =
        html ||
        `<div style="padding:12px;font-size:13px;color:var(--text-3)">${zh ? "未找到匹配的文档" : "No matching documents"}</div>`;

      this.navEl.querySelectorAll("[data-doc-path]").forEach((btn) => {
        btn.addEventListener("click", () => {
          const target = btn.dataset.docPath;
          if (this.onNavigate) this.onNavigate(target);
          else this.loadDoc(target);
        });
      });
    }

    async loadDoc(docPath, anchorHash) {
      const valid = FLAT_DOCS.find((d) => d.path === docPath);
      const target = valid ? valid.path : "architecture.md";
      this.currentDoc = target;
      this.renderNav(this.searchEl ? this.searchEl.value : "");

      if (this.crumbEl) this.crumbEl.textContent = `docs/${target}`;
      if (this.ghLinkEl) this.ghLinkEl.href = REPO_BLOB + target;

      if (this.cache.has(target)) {
        this.renderDocContent(target, this.cache.get(target), anchorHash);
        return;
      }

      if (this.bodyEl) {
        this.bodyEl.innerHTML = `<p style="color:var(--text-3)">${this.lang === "zh" ? "正在实时加载 docs/" + target + " ..." : "Loading live docs/" + target + " ..."}</p>`;
      }

      try {
        const md = await this.fetchRawDoc(target);
        this.renderDocContent(target, md, anchorHash);
      } catch (err) {
        const zh = this.lang === "zh";
        const isFileProto = location.protocol === "file:";
        if (this.bodyEl) {
          this.bodyEl.innerHTML = `
            <div class="docs-fallback">
              <h3>${zh ? "无法直接读取本地 Markdown 文件" : "Unable to fetch live Markdown file"}</h3>
              <p>${
                isFileProto
                  ? zh
                    ? "浏览器安全策略会阻止 <code>file://</code> 直接 <code>fetch('./" + target + "')</code>。为保持文档单一事实源（不存两份副本），在 GitHub Pages 或本地 HTTP 服务下会自动实时加载。"
                    : "Browser security blocks <code>fetch('./" + target + "')</code> over <code>file://</code>. Serve <code>docs/</code> over HTTP or view on GitHub Pages for live single-source docs."
                  : (zh ? "加载失败：" : "Failed to load: ") + escapeHtml(String(err))
              }</p>
              <div class="md-pre-wrap"><pre><code>python -m http.server 8080 --directory docs</code></pre></div>
              <p style="margin-top:12px">
                <a href="${REPO_BLOB + target}" target="_blank" rel="noopener">${zh ? "在 GitHub 上直接查看 " + target + " →" : "Open " + target + " on GitHub →"}</a>
              </p>
            </div>
          `;
        }
        if (this.tocEl) this.tocEl.innerHTML = "";
        this.renderPager(target);
      }
    }

    renderDocContent(docPath, mdText, anchorHash) {
      const zh = this.lang === "zh";
      const copyLbl = zh ? "复制" : "Copy";
      const copiedLbl = zh ? "已复制" : "Copied";
      const { html, toc } = renderMarkdown(mdText, docPath, copyLbl);

      if (this.bodyEl) {
        this.bodyEl.innerHTML = html;

        this.bodyEl.querySelectorAll("[data-doc-link]").forEach((a) => {
          a.addEventListener("click", (e) => {
            e.preventDefault();
            const nextDoc = a.dataset.docLink;
            const nextHash = a.dataset.docHash || "";
            if (this.onNavigate) this.onNavigate(nextDoc, nextHash);
            else this.loadDoc(nextDoc, nextHash);
          });
        });

        this.bodyEl.querySelectorAll(".md-copy-btn").forEach((btn) => {
          btn.addEventListener("click", async () => {
            try {
              await navigator.clipboard.writeText(decodeURIComponent(btn.dataset.mdCopy || ""));
              btn.textContent = copiedLbl;
              setTimeout(() => {
                btn.textContent = copyLbl;
              }, 1400);
            } catch (_) {}
          });
        });
      }

      if (this.tocEl) {
        this.tocEl.innerHTML = toc.length
          ? toc
              .map(
                (h) =>
                  `<a href="#${h.id}" class="lvl-${h.level}" data-toc-id="${h.id}">${escapeHtml(h.text)}</a>`
              )
              .join("")
          : `<span style="font-size:12px;color:var(--text-3)">${zh ? "本页无二级目录" : "No subsections"}</span>`;

        this.tocEl.querySelectorAll("[data-toc-id]").forEach((a) => {
          a.addEventListener("click", (e) => {
            e.preventDefault();
            const el = document.getElementById(a.dataset.tocId);
            if (el) el.scrollIntoView({ behavior: "smooth", block: "start" });
          });
        });

        this.setupTocSpy(toc);
      }

      this.renderPager(docPath);

      if (anchorHash) {
        const el = document.getElementById(anchorHash);
        if (el) {
          setTimeout(() => el.scrollIntoView({ behavior: "smooth", block: "start" }), 60);
          return;
        }
      }
      window.scrollTo({ top: 0, behavior: "instant" });
    }

    setupTocSpy(toc) {
      if (this.tocObserver) {
        this.tocObserver.disconnect();
        this.tocObserver = null;
      }
      if (!("IntersectionObserver" in window) || !toc.length || !this.tocEl) return;

      const links = Array.from(this.tocEl.querySelectorAll("[data-toc-id]"));
      this.tocObserver = new IntersectionObserver(
        (entries) => {
          entries.forEach((en) => {
            if (!en.isIntersecting) return;
            links.forEach((a) => a.classList.toggle("active", a.dataset.tocId === en.target.id));
          });
        },
        { rootMargin: "-12% 0px -74% 0px" }
      );

      toc.forEach((h) => {
        const el = document.getElementById(h.id);
        if (el) this.tocObserver.observe(el);
      });
    }

    renderPager(docPath) {
      if (!this.pagerEl) return;
      const idx = FLAT_DOCS.findIndex((d) => d.path === docPath);
      const prev = idx > 0 ? FLAT_DOCS[idx - 1] : null;
      const next = idx >= 0 && idx < FLAT_DOCS.length - 1 ? FLAT_DOCS[idx + 1] : null;
      const zh = this.lang === "zh";

      this.pagerEl.innerHTML = `
        ${
          prev
            ? `<button type="button" class="docs-pager-btn prev" data-pager="${prev.path}"><small>← ${zh ? "上一篇" : "Previous"}</small><strong>${zh ? prev.zh : prev.en}</strong></button>`
            : `<div></div>`
        }
        ${
          next
            ? `<button type="button" class="docs-pager-btn next" data-pager="${next.path}"><small>${zh ? "下一篇" : "Next"} →</small><strong>${zh ? next.zh : next.en}</strong></button>`
            : `<div></div>`
        }
      `;

      this.pagerEl.querySelectorAll("[data-pager]").forEach((btn) => {
        btn.addEventListener("click", () => {
          const target = btn.dataset.pager;
          if (this.onNavigate) this.onNavigate(target);
          else this.loadDoc(target);
        });
      });
    }
  }

  window.WinomeDocsPortal = WinomeDocsPortal;
})();
