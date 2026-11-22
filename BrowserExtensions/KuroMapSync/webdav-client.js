// 浏览器扩展直连 WebDAV 客户端（零外部依赖，适配坚果云、NAS、Alist/OpenList 等）

import {
  SYNC_DIR_RELATIVE,
  accountFileName,
  decryptEnvelope,
  encryptEnvelope,
  parseAccountFileName,
  validateBaseUrl
} from "./webdav-crypto.js";

function basicAuthHeader(username, password) {
  const bytes = new TextEncoder().encode(`${String(username).trim()}:${String(password)}`);
  let binary = "";
  for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]);
  return `Basic ${btoa(binary)}`;
}

function buildError(action, status, statusText) {
  if (status === 401 || status === 403) {
    return new Error(`${action}：认证失败，请检查 WebDAV 用户名和应用专用密码。`);
  }
  if (status === 429) {
    return new Error(`${action}：请求过于频繁（429），请稍后再试。`);
  }
  if (status === 404) {
    return new Error(`${action}：目标路径不存在，请检查 WebDAV 地址是否完整。`);
  }
  return new Error(`${action}：HTTP ${status} ${statusText || ""}`.trim());
}

/** 从 PROPFIND XML 响应中提取所有 kuro_<accountId>.json 的账号 ID */
export function parseAccountIdsFromPropfind(xml) {
  if (typeof xml !== "string" || !xml.trim()) return [];
  const found = new Set();
  const regex = /<(?:[a-zA-Z0-9_-]+:)?href\b[^>]*>([\s\S]*?)<\/(?:[a-zA-Z0-9_-]+:)?href>/gi;
  let match;
  while ((match = regex.exec(xml)) !== null) {
    const raw = match[1].trim();
    if (!raw || raw.endsWith("/")) continue;
    let decoded = raw;
    try { decoded = decodeURIComponent(raw); } catch { }
    const slash = decoded.lastIndexOf("/");
    const fileName = slash >= 0 ? decoded.slice(slash + 1) : decoded;
    const accountId = parseAccountFileName(fileName);
    if (accountId) found.add(accountId);
  }
  return [...found].sort();
}

export class WebDavClient {
  constructor({ baseUrl, username, password }) {
    this.baseUrl = validateBaseUrl(baseUrl);
    if (!username || !String(username).trim()) throw new Error("请输入 WebDAV 用户名。");
    if (!password) throw new Error("请输入 WebDAV 密码（坚果云请使用应用专用密码）。");
    this.syncDirUrl = new URL(SYNC_DIR_RELATIVE, this.baseUrl).href;
    this.auth = basicAuthHeader(username, password);
  }

  async _fetch(url, options = {}) {
    const headers = new Headers(options.headers || {});
    headers.set("Authorization", this.auth);
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 15000);
    try {
      return await fetch(url, {
        ...options,
        headers,
        credentials: "omit",
        cache: "no-store",
        signal: controller.signal
      });
    } catch (error) {
      if (error?.name === "AbortError") {
        throw new Error("连接 WebDAV 超时（15 秒），请检查网络或浏览器代理设置。");
      }
      throw new Error(`无法连接到 WebDAV 服务（${error?.message || "网络错误"}），请检查网络、代理或是否已授予域名权限。`);
    } finally {
      clearTimeout(timeoutId);
    }
  }

  async _mkcol(url) {
    const response = await this._fetch(url, { method: "MKCOL" });
    if ([200, 201, 204, 301, 405, 409].includes(response.status)) return;
    throw buildError("创建 WebDAV 同步目录失败", response.status, response.statusText);
  }

  async ensureSyncDirectory() {
    await this._mkcol(new URL("imao-kuro-sync/", this.baseUrl).href);
    await this._mkcol(this.syncDirUrl);
  }

  async testConnection() {
    await this.ensureSyncDirectory();
    return this.listAccounts();
  }

  async listAccounts() {
    const response = await this._fetch(this.syncDirUrl, {
      method: "PROPFIND",
      headers: {
        Depth: "1",
        "Content-Type": "application/xml; charset=utf-8"
      },
      body: '<?xml version="1.0" encoding="utf-8"?><d:propfind xmlns:d="DAV:"><d:prop><d:resourcetype/></d:prop></d:propfind>'
    });
    if (response.status === 404) return [];
    if (response.status < 200 || response.status >= 300) {
      throw buildError("读取 WebDAV 目录列表失败", response.status, response.statusText);
    }
    const xml = await response.text();
    const accountIds = parseAccountIdsFromPropfind(xml);
    const summaries = [];
    for (const accountId of accountIds) {
      const envelope = await this.tryGetEnvelope(accountId);
      if (!envelope) continue;
      summaries.push({
        accountId: envelope.accountId,
        version: envelope.version,
        updatedAtUtc: envelope.updatedAtUtc,
        sourceDevice: envelope.sourceDevice || "",
        displayName: envelope.displayName || ""
      });
    }
    summaries.sort((a, b) => a.accountId.localeCompare(b.accountId));
    return summaries;
  }

  async tryGetEnvelope(accountId) {
    const url = new URL(accountFileName(accountId), this.syncDirUrl).href;
    const response = await this._fetch(url, { method: "GET" });
    if (response.status === 404) return null;
    if (response.status < 200 || response.status >= 300) {
      throw buildError(`读取账号 ${accountId} 的云端凭证失败`, response.status, response.statusText);
    }
    const text = await response.text();
    return JSON.parse(text);
  }

  async download(accountId, passphrase) {
    const envelope = await this.tryGetEnvelope(accountId);
    if (!envelope) return null;
    const payload = await decryptEnvelope(envelope, passphrase);
    return { envelope, payload };
  }

  async upload({ accountId, token, passphrase, sourceDevice = "browser", displayName = "", userInfoJson = "" }) {
    await this.ensureSyncDirectory();
    let currentVersion = 0;
    const existing = await this.tryGetEnvelope(accountId);
    if (existing && Number.isInteger(existing.version) && existing.version > 0) {
      currentVersion = existing.version;
    }
    const envelope = await encryptEnvelope({
      accountId,
      version: currentVersion + 1,
      token,
      passphrase,
      sourceDevice,
      displayName,
      userInfoJson
    });
    const url = new URL(accountFileName(accountId), this.syncDirUrl).href;
    const response = await this._fetch(url, {
      method: "PUT",
      headers: { "Content-Type": "application/json; charset=utf-8" },
      body: JSON.stringify(envelope)
    });
    if (response.status < 200 || response.status >= 300) {
      throw buildError(`上传账号 ${accountId} 的云端凭证失败`, response.status, response.statusText);
    }
    return envelope;
  }

  async deleteAccount(accountId) {
    const url = new URL(accountFileName(accountId), this.syncDirUrl).href;
    const response = await this._fetch(url, { method: "DELETE" });
    if (response.status === 404) return;
    if (response.status < 200 || response.status >= 300) {
      throw buildError(`删除账号 ${accountId} 的云端凭证失败`, response.status, response.statusText);
    }
  }
}
