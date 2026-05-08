(() => {
  const STORAGE_KEY = "componentSorterBaseUrl";
  const DEFAULT_BASE_URL = "http://192.168.3.75:32323";
  const FALLBACK_BASE_URLS = [
    "http://192.168.3.75:32323",
    "http://192.168.3.238:32323",
  ];
  const DEFAULT_PORT = "32323";
  const CONTAINER_SELECTOR = "#smt-ui-container";
  const CODE_PATTERN = /\bC\d{4,}\b/gi;
  const TOKEN_SPLIT_PATTERN = /[^a-z0-9._+\-]+/i;
  const MAX_HISTORY = 8;
  const IGNORED_TOKENS = new Set([
    "",
    "top",
    "bottom",
    "layer",
    "smt",
    "bom",
    "lcsc",
    "none",
    "true",
    "false",
    "null",
  ]);
  const PACKAGE_CODE_PATTERN =
    /^C(?:01005|0201|0402|0603|0805|1206|1210|1806|1812|2010|2512|1005|1608|2012|3216|3225|4532|5025|6032|6432|7343|7345|7451)$/i;

  let statusRoot;
  let statusBadge;
  let statusText;
  let statusDetail;
  let statusMeta;
  let historyList;
  let settingsButton;
  let clearHistoryButton;
  let pickerOverlay;
  let pickerList;
  let pickerSummary;
  let bomEntryCache;
  let recentHistory = [];
  let currentRequestId = 0;
  let activeRow = null;

  function normalizeBaseUrl(value) {
    if (!value) {
      return "";
    }

    let normalized = String(value).trim();
    if (!normalized) {
      return "";
    }

    if (!/^https?:\/\//i.test(normalized)) {
      normalized = `http://${normalized}`;
    }

    normalized = normalized.replace(/\/+$/, "");
    return normalized;
  }

  function getSuggestedBaseUrl() {
    const currentHost = window.location.hostname;
    if (currentHost) {
      return normalizeBaseUrl(`http://${currentHost}:${DEFAULT_PORT}`);
    }
    return DEFAULT_BASE_URL;
  }

  function getBaseUrlFromQuery() {
    try {
      const params = new URLSearchParams(window.location.search);
      return normalizeBaseUrl(params.get("baseUrl"));
    } catch (error) {
      return "";
    }
  }

  function getConfiguredBaseUrl() {
    const queryBaseUrl = getBaseUrlFromQuery();
    if (queryBaseUrl) {
      return queryBaseUrl;
    }

    const explicitConfig = normalizeBaseUrl(
      window.componentSorterBridgeConfig &&
        window.componentSorterBridgeConfig.baseUrl
    );
    if (explicitConfig) {
      return explicitConfig;
    }

    const saved = normalizeBaseUrl(window.localStorage.getItem(STORAGE_KEY));
    if (saved) {
      return saved;
    }

    if (DEFAULT_BASE_URL) {
      return normalizeBaseUrl(DEFAULT_BASE_URL);
    }

    if (window.location.protocol !== "file:" && window.location.hostname) {
      return getSuggestedBaseUrl();
    }

    return normalizeBaseUrl(DEFAULT_BASE_URL);
  }

  function getCandidateBaseUrls() {
    const candidates = [];
    const push = (value) => {
      const normalized = normalizeBaseUrl(value);
      if (normalized && !candidates.includes(normalized)) {
        candidates.push(normalized);
      }
    };

    const queryBaseUrl = getBaseUrlFromQuery();
    const explicitConfig = normalizeBaseUrl(
      window.componentSorterBridgeConfig &&
        window.componentSorterBridgeConfig.baseUrl
    );
    const saved = normalizeBaseUrl(window.localStorage.getItem(STORAGE_KEY));

    push(queryBaseUrl);
    push(explicitConfig);
    push(saved);
    push(DEFAULT_BASE_URL);
    push(getSuggestedBaseUrl());
    FALLBACK_BASE_URLS.forEach(push);

    return candidates;
  }

  function saveBaseUrl(value) {
    const normalized = normalizeBaseUrl(value);
    if (!normalized) {
      window.localStorage.removeItem(STORAGE_KEY);
      return "";
    }

    window.localStorage.setItem(STORAGE_KEY, normalized);
    return normalized;
  }

  function formatNow() {
    return new Date().toLocaleTimeString("zh-CN", {
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
    });
  }

  function getToneLabel(tone) {
    if (tone === "success") {
      return "成功";
    }
    if (tone === "warn") {
      return "提示";
    }
    if (tone === "error") {
      return "异常";
    }
    if (tone === "working") {
      return "处理中";
    }
    return "联动中";
  }

  function ensureStatusPanel() {
    if (statusRoot) {
      return;
    }

    statusRoot = document.createElement("div");
    statusRoot.id = "component-sorter-bridge-status";
    statusRoot.innerHTML = `
      <div class="csb-header">
        <div class="csb-brand">
          <span class="csb-badge">联动中</span>
          <span class="csb-title">Interactive BOM 双击亮灯</span>
        </div>
        <div class="csb-actions">
          <button type="button" class="csb-clear">清空记录</button>
          <button type="button" class="csb-settings">设置地址</button>
        </div>
      </div>
      <div class="csb-body">
        <div class="csb-text"></div>
        <div class="csb-detail"></div>
        <div class="csb-meta"></div>
      </div>
      <div class="csb-history">
        <div class="csb-history-title">最近查找</div>
        <div class="csb-history-list"></div>
      </div>
    `;

    pickerOverlay = document.createElement("div");
    pickerOverlay.id = "component-sorter-bridge-picker";
    pickerOverlay.hidden = true;
    pickerOverlay.innerHTML = `
      <div class="csb-picker-mask"></div>
      <div class="csb-picker-dialog" role="dialog" aria-modal="true">
        <div class="csb-picker-header">
          <div class="csb-picker-title">请选择要查找的料号</div>
          <button type="button" class="csb-picker-close">关闭</button>
        </div>
        <div class="csb-picker-summary"></div>
        <div class="csb-picker-list"></div>
      </div>
    `;

    const style = document.createElement("style");
    style.textContent = `
      #component-sorter-bridge-status {
        position: fixed;
        right: 16px;
        bottom: 16px;
        z-index: 2147483647;
        width: 380px;
        max-width: calc(100vw - 24px);
        border-radius: 16px;
        box-shadow: 0 18px 48px rgba(0, 0, 0, 0.24);
        font-family: "Microsoft YaHei", "Segoe UI", Arial, sans-serif;
        background: rgba(15, 23, 42, 0.94);
        color: #f8fafc;
        backdrop-filter: blur(12px);
        overflow: hidden;
      }

      #component-sorter-bridge-status[data-tone="success"] {
        box-shadow: 0 18px 48px rgba(6, 95, 70, 0.26);
      }

      #component-sorter-bridge-status[data-tone="warn"] {
        box-shadow: 0 18px 48px rgba(146, 64, 14, 0.28);
      }

      #component-sorter-bridge-status[data-tone="error"] {
        box-shadow: 0 18px 48px rgba(127, 29, 29, 0.28);
      }

      #component-sorter-bridge-status .csb-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 10px;
        padding: 12px 14px;
        background: rgba(255, 255, 255, 0.06);
        border-bottom: 1px solid rgba(255, 255, 255, 0.08);
      }

      #component-sorter-bridge-status .csb-brand {
        display: flex;
        align-items: center;
        gap: 8px;
        min-width: 0;
      }

      #component-sorter-bridge-status .csb-badge {
        display: inline-flex;
        align-items: center;
        justify-content: center;
        min-width: 54px;
        padding: 3px 8px;
        border-radius: 999px;
        background: rgba(59, 130, 246, 0.22);
        color: #dbeafe;
        font-size: 11px;
        font-weight: 700;
      }

      #component-sorter-bridge-status[data-tone="success"] .csb-badge {
        background: rgba(34, 197, 94, 0.24);
        color: #dcfce7;
      }

      #component-sorter-bridge-status[data-tone="warn"] .csb-badge {
        background: rgba(251, 191, 36, 0.24);
        color: #fef3c7;
      }

      #component-sorter-bridge-status[data-tone="error"] .csb-badge {
        background: rgba(248, 113, 113, 0.24);
        color: #fee2e2;
      }

      #component-sorter-bridge-status[data-tone="working"] .csb-badge {
        background: rgba(56, 189, 248, 0.24);
        color: #e0f2fe;
      }

      #component-sorter-bridge-status .csb-title {
        font-size: 14px;
        font-weight: 700;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
      }

      #component-sorter-bridge-status .csb-actions {
        display: flex;
        gap: 8px;
        flex-shrink: 0;
      }

      #component-sorter-bridge-status .csb-settings,
      #component-sorter-bridge-status .csb-clear,
      #component-sorter-bridge-picker button,
      #component-sorter-bridge-status .csb-history-item {
        border: none;
        border-radius: 10px;
        cursor: pointer;
        font-family: inherit;
      }

      #component-sorter-bridge-status .csb-settings,
      #component-sorter-bridge-status .csb-clear {
        padding: 6px 10px;
        color: #0f172a;
        background: #f8fafc;
        font-size: 12px;
        font-weight: 600;
      }

      #component-sorter-bridge-status .csb-body {
        padding: 14px;
      }

      #component-sorter-bridge-status .csb-text {
        font-size: 18px;
        line-height: 1.4;
        font-weight: 700;
      }

      #component-sorter-bridge-status .csb-detail {
        margin-top: 8px;
        font-size: 13px;
        line-height: 1.6;
        opacity: 0.92;
        word-break: break-word;
      }

      #component-sorter-bridge-status .csb-meta {
        margin-top: 8px;
        font-size: 12px;
        opacity: 0.72;
      }

      #component-sorter-bridge-status .csb-history {
        padding: 0 14px 14px;
      }

      #component-sorter-bridge-status .csb-history-title {
        margin-bottom: 8px;
        font-size: 12px;
        font-weight: 700;
        opacity: 0.8;
      }

      #component-sorter-bridge-status .csb-history-list {
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
      }

      #component-sorter-bridge-status .csb-history-empty {
        font-size: 12px;
        opacity: 0.62;
      }

      #component-sorter-bridge-status .csb-history-item {
        padding: 8px 10px;
        background: rgba(255, 255, 255, 0.08);
        color: #f8fafc;
        font-size: 12px;
        line-height: 1.4;
        text-align: left;
      }

      #component-sorter-bridge-status .csb-history-item:hover {
        background: rgba(255, 255, 255, 0.14);
      }

      #component-sorter-bridge-status .csb-history-item[data-state="success"] {
        background: rgba(34, 197, 94, 0.18);
        box-shadow: inset 0 0 0 1px rgba(134, 239, 172, 0.26);
      }

      #component-sorter-bridge-status .csb-history-item[data-state="warn"] {
        background: rgba(245, 158, 11, 0.18);
        box-shadow: inset 0 0 0 1px rgba(253, 224, 71, 0.24);
      }

      #component-sorter-bridge-status .csb-history-item[data-state="error"] {
        background: rgba(239, 68, 68, 0.18);
        box-shadow: inset 0 0 0 1px rgba(252, 165, 165, 0.24);
      }

      #component-sorter-bridge-status .csb-history-code {
        display: block;
        font-weight: 700;
      }

      #component-sorter-bridge-status .csb-history-time {
        display: block;
        margin-top: 2px;
        opacity: 0.72;
      }

      #component-sorter-bridge-status .csb-history-state {
        display: block;
        margin-top: 4px;
        font-size: 11px;
        font-weight: 700;
        opacity: 0.9;
      }

      #smt-ui-container tr.csb-row-active > td,
      #smt-ui-container tr.csb-row-active > th {
        background: rgba(59, 130, 246, 0.14) !important;
        box-shadow: inset 0 -1px 0 rgba(96, 165, 250, 0.18),
          inset 0 1px 0 rgba(96, 165, 250, 0.18);
      }

      #smt-ui-container tr.csb-row-active {
        outline: 2px solid rgba(59, 130, 246, 0.78);
        outline-offset: -2px;
      }

      #smt-ui-container tr.csb-row-success > td,
      #smt-ui-container tr.csb-row-success > th {
        background: rgba(34, 197, 94, 0.18) !important;
        box-shadow: inset 0 -1px 0 rgba(134, 239, 172, 0.24),
          inset 0 1px 0 rgba(134, 239, 172, 0.24);
      }

      #smt-ui-container tr.csb-row-success {
        outline: 2px solid rgba(34, 197, 94, 0.9);
        outline-offset: -2px;
      }

      #smt-ui-container tr.csb-row-warn > td,
      #smt-ui-container tr.csb-row-warn > th {
        background: rgba(245, 158, 11, 0.18) !important;
        box-shadow: inset 0 -1px 0 rgba(253, 224, 71, 0.24),
          inset 0 1px 0 rgba(253, 224, 71, 0.24);
      }

      #smt-ui-container tr.csb-row-warn {
        outline: 2px solid rgba(245, 158, 11, 0.9);
        outline-offset: -2px;
      }

      #component-sorter-bridge-picker[hidden] {
        display: none;
      }

      #component-sorter-bridge-picker {
        position: fixed;
        inset: 0;
        z-index: 2147483646;
        font-family: "Microsoft YaHei", "Segoe UI", Arial, sans-serif;
      }

      #component-sorter-bridge-picker .csb-picker-mask {
        position: absolute;
        inset: 0;
        background: rgba(2, 6, 23, 0.58);
      }

      #component-sorter-bridge-picker .csb-picker-dialog {
        position: relative;
        width: min(540px, calc(100vw - 24px));
        margin: 8vh auto 0;
        border-radius: 16px;
        background: #ffffff;
        color: #0f172a;
        box-shadow: 0 20px 60px rgba(15, 23, 42, 0.28);
        overflow: hidden;
      }

      #component-sorter-bridge-picker .csb-picker-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        padding: 14px 16px;
        border-bottom: 1px solid #e2e8f0;
      }

      #component-sorter-bridge-picker .csb-picker-title {
        font-size: 16px;
        font-weight: 700;
      }

      #component-sorter-bridge-picker .csb-picker-close {
        padding: 6px 10px;
        background: #e2e8f0;
        color: #0f172a;
        font-size: 12px;
        font-weight: 700;
      }

      #component-sorter-bridge-picker .csb-picker-summary {
        padding: 12px 16px 0;
        font-size: 13px;
        line-height: 1.6;
        color: #475569;
      }

      #component-sorter-bridge-picker .csb-picker-list {
        display: grid;
        gap: 10px;
        padding: 16px;
        max-height: 52vh;
        overflow-y: auto;
      }

      #component-sorter-bridge-picker .csb-picker-option {
        padding: 12px 14px;
        border: 1px solid #cbd5e1;
        border-radius: 12px;
        background: #f8fafc;
        text-align: left;
      }

      #component-sorter-bridge-picker .csb-picker-option:hover {
        background: #eef2ff;
        border-color: #93c5fd;
      }

      #component-sorter-bridge-picker .csb-picker-code {
        display: block;
        font-size: 15px;
        font-weight: 700;
      }

      #component-sorter-bridge-picker .csb-picker-desc {
        display: block;
        margin-top: 4px;
        font-size: 12px;
        color: #475569;
        line-height: 1.5;
      }
    `;

    document.head.appendChild(style);
    document.body.appendChild(statusRoot);
    document.body.appendChild(pickerOverlay);

    statusBadge = statusRoot.querySelector(".csb-badge");
    statusText = statusRoot.querySelector(".csb-text");
    statusDetail = statusRoot.querySelector(".csb-detail");
    statusMeta = statusRoot.querySelector(".csb-meta");
    historyList = statusRoot.querySelector(".csb-history-list");
    settingsButton = statusRoot.querySelector(".csb-settings");
    clearHistoryButton = statusRoot.querySelector(".csb-clear");
    pickerList = pickerOverlay.querySelector(".csb-picker-list");
    pickerSummary = pickerOverlay.querySelector(".csb-picker-summary");

    settingsButton.addEventListener("click", () => {
      promptForBaseUrl(true);
    });
    clearHistoryButton.addEventListener("click", () => {
      recentHistory = [];
      renderHistory();
      setStatus("已清空最近查找记录", "info", "新的双击查找结果会继续出现在这里");
    });
    pickerOverlay.querySelector(".csb-picker-close").addEventListener("click", closePicker);
    pickerOverlay.querySelector(".csb-picker-mask").addEventListener("click", closePicker);

    renderHistory();
  }

  function setStatus(message, tone = "info", detail = "", meta = "") {
    ensureStatusPanel();
    statusRoot.dataset.tone = tone;
    statusBadge.textContent = getToneLabel(tone);
    statusText.textContent = message;
    statusDetail.textContent = detail || "";
    statusMeta.textContent = meta || `更新时间：${formatNow()}`;
  }

  function renderHistory() {
    ensureStatusPanel();
    historyList.innerHTML = "";

    if (!recentHistory.length) {
      const empty = document.createElement("div");
      empty.className = "csb-history-empty";
      empty.textContent = "还没有查找记录，双击 BOM 行后会显示在这里。";
      historyList.appendChild(empty);
      return;
    }

    recentHistory.forEach((entry) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "csb-history-item";
      button.dataset.state = entry.status || "info";
      button.innerHTML = `
        <span class="csb-history-code">${entry.partNumber}</span>
        <span class="csb-history-time">${entry.time}</span>
        <span class="csb-history-state">${entry.statusLabel || "已查找"}</span>
      `;
      button.addEventListener("click", () => {
        requestHighlight(entry.partNumber, {
          source: "history",
          rowSummary: entry.rowSummary || "",
        });
      });
      historyList.appendChild(button);
    });
  }

  function pushHistory(partNumber, rowSummary, status) {
    recentHistory = recentHistory.filter((entry) => entry.partNumber !== partNumber);
    recentHistory.unshift({
      partNumber,
      rowSummary: normalizeText(rowSummary).slice(0, 120),
      time: formatNow(),
      status,
      statusLabel:
        status === "success"
          ? "已找到"
          : status === "warn"
            ? "未找到"
            : status === "error"
              ? "请求异常"
              : "已查找",
    });
    if (recentHistory.length > MAX_HISTORY) {
      recentHistory = recentHistory.slice(0, MAX_HISTORY);
    }
    renderHistory();
  }

  function clearActiveRow() {
    if (activeRow && activeRow.isConnected) {
      activeRow.classList.remove("csb-row-active");
      activeRow.classList.remove("csb-row-success");
      activeRow.classList.remove("csb-row-warn");
    }
    activeRow = null;
  }

  function setActiveRow(row) {
    clearActiveRow();
    if (!row || !(row instanceof Element)) {
      return;
    }
    row.classList.add("csb-row-active");
    activeRow = row;
    if (typeof row.scrollIntoView === "function") {
      row.scrollIntoView({
        block: "nearest",
        inline: "nearest",
      });
    }
  }

  function setActiveRowResult(resultTone) {
    if (!activeRow || !activeRow.isConnected) {
      return;
    }

    activeRow.classList.remove("csb-row-active");
    activeRow.classList.remove("csb-row-success");
    activeRow.classList.remove("csb-row-warn");

    if (resultTone === "success") {
      activeRow.classList.add("csb-row-success");
      return;
    }

    if (resultTone === "warn") {
      activeRow.classList.add("csb-row-warn");
      return;
    }

    activeRow.classList.add("csb-row-active");
  }

  function promptForBaseUrl(forcePrompt = false) {
    const current = getConfiguredBaseUrl() || getSuggestedBaseUrl();
    if (!forcePrompt && current) {
      return current;
    }

    const value = window.prompt(
      "请输入分拣箱地址，例如 http://192.168.3.75:32323",
      current
    );

    if (value === null) {
      return "";
    }

    const normalized = saveBaseUrl(value);
    if (normalized) {
      setStatus("分拣箱地址已更新", "success", normalized);
    } else {
      setStatus("分拣箱地址无效", "warn", "请重新设置一个有效地址");
    }
    return normalized;
  }

  function normalizeText(text) {
    return String(text || "")
      .replace(/\s+/g, " ")
      .trim();
  }

  function extractCodesFromText(text) {
    const normalized = normalizeText(text);
    if (!normalized) {
      return [];
    }
    const matches = normalized.match(CODE_PATTERN) || [];
    return Array.from(new Set(matches.map((item) => item.toUpperCase())));
  }

  function isLikelyPackageCode(code) {
    return PACKAGE_CODE_PATTERN.test(String(code || "").toUpperCase());
  }

  function isExplicitPartNumber(code) {
    const normalized = String(code || "").toUpperCase();
    if (!normalized) {
      return false;
    }
    if (isLikelyPackageCode(normalized)) {
      return false;
    }
    return /^C\d{4,}$/.test(normalized);
  }

  function collectStringsDeep(value, output) {
    if (value == null) {
      return;
    }

    if (typeof value === "string" || typeof value === "number" || typeof value === "boolean") {
      const normalized = normalizeText(value);
      if (normalized) {
        output.push(normalized);
      }
      return;
    }

    if (Array.isArray(value)) {
      value.forEach((item) => collectStringsDeep(item, output));
      return;
    }

    if (typeof value === "object") {
      Object.entries(value).forEach(([key, item]) => {
        output.push(normalizeText(key));
        collectStringsDeep(item, output);
      });
    }
  }

  function extractCodesFromAny(value) {
    const strings = [];
    collectStringsDeep(value, strings);
    const codes = new Set();
    strings.forEach((text) => {
      extractCodesFromText(text).forEach((code) => codes.add(code));
    });
    return Array.from(codes);
  }

  function extractExplicitCodesFromBomEntry(entryKey, entryValue) {
    const codes = new Set();
    const addCodes = (value) => {
      extractCodesFromText(value).forEach((code) => {
        if (isExplicitPartNumber(code)) {
          codes.add(code);
        }
      });
    };

    const keyPrefix = normalizeText(entryKey).split(",")[0];
    addCodes(keyPrefix);

    if (entryValue && typeof entryValue === "object") {
      Object.entries(entryValue).forEach(([fieldKey, fieldValue]) => {
        const normalizedFieldKey = String(fieldKey || "").toLowerCase();
        if (
          normalizedFieldKey.includes("component_code") ||
          normalizedFieldKey.includes("customer_component_code") ||
          normalizedFieldKey.includes("lcsc_component_code") ||
          normalizedFieldKey.includes("lcsc")
        ) {
          addCodes(fieldValue);
        }
      });
    }

    return Array.from(codes);
  }

  function tokenize(text) {
    return normalizeText(text)
      .toLowerCase()
      .split(TOKEN_SPLIT_PATTERN)
      .map((item) => item.trim())
      .filter((item) => item.length >= 2 && !IGNORED_TOKENS.has(item));
  }

  function buildBomEntryCache() {
    if (bomEntryCache) {
      return bomEntryCache;
    }

    bomEntryCache = [];

    try {
      const files = window.files;
      if (!files || typeof files.bom_merge !== "string") {
        return bomEntryCache;
      }

      const outer = JSON.parse(files.bom_merge);
      const inner = outer && typeof outer.data === "string" ? JSON.parse(outer.data) : null;
      const compInfo = inner && inner.comp_info ? inner.comp_info : null;
      if (!compInfo || typeof compInfo !== "object") {
        return bomEntryCache;
      }

      Object.entries(compInfo).forEach(([entryKey, entryValue]) => {
        const strings = [normalizeText(entryKey)];
        collectStringsDeep(entryValue, strings);

        const codeList = extractExplicitCodesFromBomEntry(entryKey, entryValue);
        if (!codeList.length) {
          return;
        }

        const normalizedStrings = Array.from(
          new Set(strings.map((item) => normalizeText(item)).filter(Boolean))
        );
        const tokens = Array.from(new Set(normalizedStrings.flatMap((item) => tokenize(item))));
        const summary = normalizedStrings
          .filter((item) => !extractCodesFromText(item).length)
          .slice(0, 4)
          .join(" | ");

        codeList.forEach((code) => {
          bomEntryCache.push({
            code,
            searchText: normalizedStrings.join(" | ").toLowerCase(),
            tokens,
            summary,
          });
        });
      });
    } catch (error) {
      console.warn("component sorter bridge: failed to parse bom_merge", error);
    }

    return bomEntryCache;
  }

  function getRowTexts(row) {
    const candidateTexts = [];
    candidateTexts.push(row.innerText || "");
    candidateTexts.push(row.textContent || "");

    const cells = row.querySelectorAll("td, th, [title], [data-title]");
    cells.forEach((cell) => {
      candidateTexts.push(cell.innerText || "");
      candidateTexts.push(cell.textContent || "");
      candidateTexts.push(cell.getAttribute("title") || "");
      candidateTexts.push(cell.getAttribute("data-title") || "");
    });

    return candidateTexts.map((item) => normalizeText(item)).filter(Boolean);
  }

  function extractCandidateCodesFromRow(row) {
    const rowTexts = getRowTexts(row);
    const bomEntries = buildBomEntryCache();
    const knownPartNumbers = new Set(bomEntries.map((entry) => entry.code));
    const directCodes = Array.from(
      new Set(
        rowTexts
          .flatMap((text) => extractCodesFromText(text))
          .filter((code) => knownPartNumbers.has(code) || isExplicitPartNumber(code))
      )
    );

    if (directCodes.length) {
      return directCodes.map((code) => ({
        code,
        source: "row",
        score: 100,
        summary: normalizeText(rowTexts.join(" | ")).slice(0, 180),
      }));
    }

    const normalizedRowText = normalizeText(rowTexts.join(" "));
    if (!normalizedRowText) {
      return [];
    }

    const directText = normalizedRowText.toLowerCase();
    const rowTokens = tokenize(normalizedRowText);
    const scored = [];

    bomEntries.forEach((entry) => {
      let score = 0;

      if (entry.searchText.includes(directText) || directText.includes(entry.searchText)) {
        score += 10;
      }

      rowTokens.forEach((token) => {
        if (entry.tokens.includes(token) || entry.searchText.includes(token)) {
          score += token.length >= 4 ? 3 : 1;
        }
      });

      if (score >= 4) {
        scored.push({
          code: entry.code,
          source: "bom_merge",
          score,
          summary: entry.summary || normalizeText(rowTexts.join(" | ")).slice(0, 180),
        });
      }
    });

    const bestByCode = new Map();
    scored.forEach((item) => {
      const existing = bestByCode.get(item.code);
      if (!existing || item.score > existing.score) {
        bestByCode.set(item.code, item);
      }
    });

    return Array.from(bestByCode.values())
      .sort((a, b) => b.score - a.score || a.code.localeCompare(b.code))
      .slice(0, 5);
  }

  function closePicker() {
    if (!pickerOverlay) {
      return;
    }
    pickerOverlay.hidden = true;
    pickerList.innerHTML = "";
    pickerSummary.textContent = "";
  }

  function openPicker(candidates, rowSummary, rowElement) {
    ensureStatusPanel();
    pickerOverlay.hidden = false;
    pickerList.innerHTML = "";
    pickerSummary.textContent =
      `当前双击行匹配到了多个可能的料号，请手动选择。原始行内容：${normalizeText(rowSummary).slice(0, 180)}`;

    candidates.forEach((candidate) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "csb-picker-option";
      button.innerHTML = `
        <span class="csb-picker-code">${candidate.code}</span>
        <span class="csb-picker-desc">来源：${candidate.source === "row" ? "表格文本" : "bom_merge 兜底"}${candidate.summary ? ` | ${candidate.summary}` : ""}</span>
      `;
      button.addEventListener("click", () => {
        closePicker();
        requestHighlight(candidate.code, {
          source: "picker",
          rowSummary,
          rowElement,
        });
      });
      pickerList.appendChild(button);
    });
  }

  async function requestHighlight(partNumber, options = {}) {
    const candidateBaseUrls = getCandidateBaseUrls();
    if (!candidateBaseUrls.length) {
      const prompted = promptForBaseUrl(false);
      if (prompted) {
        candidateBaseUrls.push(prompted);
      }
    }

    if (!candidateBaseUrls.length) {
      setStatus("还没有配置分拣箱地址", "warn", "点击“设置地址”后再双击 BOM 行");
      return;
    }

    closePicker();
    if (options.rowElement) {
      setActiveRow(options.rowElement);
    }
    const requestId = ++currentRequestId;
    const sourceText = options.source ? `来源：${options.source}` : "来源：双击 BOM";
    setStatus(
      `正在查找 ${partNumber}`,
      "working",
      `${partNumber} -> ${candidateBaseUrls[0]}/api/find`,
      `${sourceText} | 共尝试 ${candidateBaseUrls.length} 个地址`
    );

    let lastErrorDetail = "";
    let lastTriedBaseUrl = candidateBaseUrls[0];

    for (let index = 0; index < candidateBaseUrls.length; index += 1) {
      const baseUrl = candidateBaseUrls[index];
      lastTriedBaseUrl = baseUrl;

      if (index > 0) {
        setStatus(
          `正在重试 ${partNumber}`,
          "working",
          `${partNumber} -> ${baseUrl}/api/find`,
          `${sourceText} | 第 ${index + 1}/${candidateBaseUrls.length} 次尝试`
        );
      }

      let response;
      try {
        response = await fetch(`${baseUrl}/api/find`, {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
          },
          body: JSON.stringify({
            part_number: partNumber,
          }),
        });
      } catch (error) {
        lastErrorDetail = error instanceof Error ? error.message : String(error);
        continue;
      }

      let payload = null;
      try {
        payload = await response.json();
      } catch (error) {
        lastErrorDetail = error instanceof Error ? error.message : String(error);
        continue;
      }

      if (requestId !== currentRequestId) {
        return;
      }

      if (!response.ok) {
        lastErrorDetail = payload && payload.error ? payload.error : `HTTP ${response.status}`;
        continue;
      }

      saveBaseUrl(baseUrl);

      if (payload && payload.success) {
      const groupText =
        payload.group && Number(payload.group) > 0
          ? `第 ${payload.group} 组`
          : "当前组";
      const ledText =
        payload.led_index !== undefined ? `LED ${payload.led_index}` : "LED 未返回";
      pushHistory(partNumber, options.rowSummary || "", "success");
      setActiveRowResult("success");
      setStatus(
        `已点亮 ${partNumber}`,
        "success",
        `${groupText} | ${ledText}`,
        `已连接：${baseUrl} | ${formatNow()}`
      );
      return;
      }

      setStatus(
        "分拣箱中未找到该料号",
        "warn",
        payload && payload.error ? `${partNumber} | ${payload.error}` : partNumber,
        `已连接：${baseUrl} | ${formatNow()}`
      );
      setActiveRowResult("warn");
      pushHistory(partNumber, options.rowSummary || "", "warn");
      return;
    }

    if (requestId !== currentRequestId) {
      return;
    }

    setStatus(
      "无法连接分拣箱",
      "error",
      lastErrorDetail || "所有候选地址都无法连接",
      `最后尝试地址：${lastTriedBaseUrl}`
    );
    setActiveRowResult("active");
    pushHistory(partNumber, options.rowSummary || "", "error");
  }

  function handleDoubleClick(event) {
    const target = event.target;
    if (!(target instanceof Element)) {
      return;
    }

    const container = target.closest(CONTAINER_SELECTOR);
    if (!container) {
      return;
    }

    const row = target.closest("tr");
    if (!row) {
      return;
    }
    setActiveRow(row);

    const rowSummary = normalizeText(getRowTexts(row).join(" | "));
    const candidates = extractCandidateCodesFromRow(row);
    if (!candidates.length) {
      setStatus(
        "没有识别到 LCSC 料号",
        "warn",
        "请双击包含料号的 BOM 行；脚本会先读表格文本，再尝试用 bom_merge 数据兜底匹配",
        rowSummary.slice(0, 160)
      );
      return;
    }

    if (candidates.length === 1) {
      requestHighlight(candidates[0].code, {
        source: candidates[0].source,
        rowSummary,
        rowElement: row,
      });
      return;
    }

    setStatus(
      "发现多个可能料号",
      "warn",
      candidates.map((item) => item.code).join(" / "),
      "请在弹窗里选择你要点亮的元器件"
    );
    openPicker(candidates, rowSummary, row);
  }

  function installBridge() {
    ensureStatusPanel();
    document.addEventListener("dblclick", handleDoubleClick, true);

    const baseUrl = getConfiguredBaseUrl();
    setStatus(
      "Interactive BOM 双击亮灯已启用",
      "info",
      baseUrl
        ? `当前分拣箱地址：${baseUrl}`
        : "首次使用请点击“设置地址”或直接双击后输入地址",
      "支持最近查找记录与多料号手动选择"
    );
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", installBridge, { once: true });
  } else {
    installBridge();
  }
})();
