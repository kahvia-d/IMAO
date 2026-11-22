// 跨端统一的 WebDAV 凭证加密信封模块（与 PC 端 C# KuroWebDavCrypto 逐字节互通）
// 算法：PBKDF2-HMAC-SHA256 (210,000 次迭代) + AES-256-GCM (12 字节 Nonce, 16 字节 Tag)

export const SCHEMA_VERSION = 1;
export const KDF_ALG = "PBKDF2-SHA256";
export const CIPHER_ALG = "AES-256-GCM";
export const DEFAULT_ITERATIONS = 210000;
export const MIN_ITERATIONS = 100000;
export const MAX_ITERATIONS = 1000000;
export const SALT_BYTES = 16;
export const NONCE_BYTES = 12;
export const TAG_BYTES = 16;
export const MAX_TOKEN_LENGTH = 16384;
export const MAX_USER_INFO_LENGTH = 32768;
export const CONFIG_BUNDLE_PREFIX = "imao-webdav-v1:";
export const SYNC_DIR_RELATIVE = "imao-kuro-sync/v1/";

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/** 校验是否为合法的纯数字库街区账号 ID（1–24 位） */
export function isAccountId(value) {
  return typeof value === "string" && /^[0-9]{1,24}$/.test(value);
}

/** 生成账号对应的 WebDAV 文件名 */
export function accountFileName(accountId) {
  if (!isAccountId(accountId)) throw new Error("无效的库街区账号 ID（仅支持纯数字）。");
  return `kuro_${accountId}.json`;
}

/** 从文件名解析库街区账号 ID，不匹配返回空字符串 */
export function parseAccountFileName(fileName) {
  if (typeof fileName !== "string") return "";
  const match = /^kuro_([0-9]{1,24})\.json$/.exec(fileName);
  return match ? match[1] : "";
}

/** 判断主机是否属于本机回环或局域网私有地址 */
export function isPrivateOrLoopbackHost(host) {
  if (!host) return false;
  const lower = host.toLowerCase();
  if (lower === "localhost" || lower === "127.0.0.1" || lower === "::1" || lower === "[::1]" || lower.endsWith(".local")) {
    return true;
  }
  const ipv4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(lower);
  if (!ipv4) return false;
  const [a, b] = [Number(ipv4[1]), Number(ipv4[2])];
  return a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || (a === 169 && b === 254);
}

/** 校验并规范化 WebDAV 基础地址（末尾补齐斜杠；公网强制 HTTPS） */
export function validateBaseUrl(rawUrl) {
  if (typeof rawUrl !== "string" || !rawUrl.trim()) {
    throw new Error("请输入 WebDAV 服务地址。");
  }
  let url;
  try {
    url = new URL(rawUrl.trim());
  } catch {
    throw new Error("WebDAV 地址格式无效，请输入完整 URL（如 https://dav.jianguoyun.com/dav/）。");
  }
  if (url.protocol !== "https:" && url.protocol !== "http:") {
    throw new Error("WebDAV 地址仅支持 https://（局域网或本机可放行 http://）。");
  }
  if (url.protocol === "http:" && !isPrivateOrLoopbackHost(url.hostname)) {
    throw new Error("公网 WebDAV 地址必须使用 https://，以防凭据明文传输。");
  }
  if (url.search || url.hash) {
    throw new Error("WebDAV 地址不应包含查询参数或锚点。");
  }
  const href = url.href.endsWith("/") ? url.href : `${url.href}/`;
  return href;
}

/** 返回 WebDAV 域名对应的 Chrome 权限匹配模式（如 https://dav.jianguoyun.com/*） */
export function originPatternForBaseUrl(rawUrl) {
  const url = new URL(validateBaseUrl(rawUrl));
  return `${url.origin}/*`;
}

/** 构造与 C# 一致的附加认证数据（AAD） */
export function buildAad(accountId, version) {
  return encoder.encode(`imao-kuro-webdav-v1|${accountId}|${version}`);
}

export function bytesToBase64(bytes) {
  let binary = "";
  for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]);
  return btoa(binary);
}

export function base64ToBytes(base64) {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

async function deriveAesKey(passphrase, salt, iterations) {
  const keyMaterial = await crypto.subtle.importKey(
    "raw",
    encoder.encode(passphrase),
    "PBKDF2",
    false,
    ["deriveKey"]
  );
  return crypto.subtle.deriveKey(
    { name: "PBKDF2", salt, iterations, hash: "SHA-256" },
    keyMaterial,
    { name: "AES-GCM", length: 256 },
    false,
    ["encrypt", "decrypt"]
  );
}

/** 使用显式材料加密（供跨端测试向量校验） */
export async function encryptEnvelopeWithMaterial({
  accountId,
  version,
  token,
  passphrase,
  salt,
  nonce,
  iterations = DEFAULT_ITERATIONS,
  sourceDevice = "browser",
  displayName = "",
  userInfoJson = "",
  timestampUtc = new Date().toISOString()
}) {
  if (!isAccountId(accountId)) throw new Error("无效的库街区账号 ID（仅支持纯数字）。");
  if (!Number.isInteger(version) || version <= 0) throw new Error("版本号必须为正整数。");
  if (typeof token !== "string" || !token.trim() || token.length > MAX_TOKEN_LENGTH) {
    throw new Error("无效的库街区登录凭据。");
  }
  if (typeof passphrase !== "string" || !passphrase) throw new Error("同步加密口令不能为空。");
  if (salt.length !== SALT_BYTES || nonce.length !== NONCE_BYTES) throw new Error("加密随机参数长度无效。");
  if (iterations < MIN_ITERATIONS || iterations > MAX_ITERATIONS) throw new Error("PBKDF2 迭代次数超出安全范围。");
  const safeUserInfo = typeof userInfoJson === "string" ? userInfoJson : "";
  if (safeUserInfo.length > MAX_USER_INFO_LENGTH) throw new Error("用户信息载荷超过长度限制。");

  const payloadJson = JSON.stringify({
    accountId,
    token,
    userInfoJson: safeUserInfo,
    savedAtUtc: timestampUtc
  });
  const key = await deriveAesKey(passphrase, salt, iterations);
  const encryptedBuffer = await crypto.subtle.encrypt(
    { name: "AES-GCM", iv: nonce, additionalData: buildAad(accountId, version), tagLength: 128 },
    key,
    encoder.encode(payloadJson)
  );
  return {
    schemaVersion: SCHEMA_VERSION,
    accountId,
    version,
    updatedAtUtc: timestampUtc,
    sourceDevice: String(sourceDevice || "browser").trim().slice(0, 32) || "browser",
    displayName: String(displayName || "").trim().slice(0, 64),
    kdf: {
      alg: KDF_ALG,
      iterations,
      salt: bytesToBase64(salt)
    },
    cipher: {
      alg: CIPHER_ALG,
      nonce: bytesToBase64(nonce),
      ciphertext: bytesToBase64(new Uint8Array(encryptedBuffer))
    }
  };
}

/** 加密生成云端信封 */
export async function encryptEnvelope(params) {
  const salt = crypto.getRandomValues(new Uint8Array(SALT_BYTES));
  const nonce = crypto.getRandomValues(new Uint8Array(NONCE_BYTES));
  return encryptEnvelopeWithMaterial({ ...params, salt, nonce });
}

/** 解密云端信封并校验 AAD 与账号一致性 */
export async function decryptEnvelope(envelope, passphrase) {
  if (!envelope || typeof envelope !== "object") throw new Error("云端凭证信封格式无效。");
  if (envelope.schemaVersion !== SCHEMA_VERSION) {
    throw new Error(`不支持的云端凭证信封版本：${envelope.schemaVersion}。`);
  }
  if (!isAccountId(envelope.accountId)) throw new Error("云端凭证账号 ID 无效。");
  if (!Number.isInteger(envelope.version) || envelope.version <= 0) throw new Error("云端凭证版本号无效。");
  if (typeof passphrase !== "string" || !passphrase) throw new Error("同步加密口令不能为空。");
  if (envelope.kdf?.alg !== KDF_ALG || envelope.cipher?.alg !== CIPHER_ALG) {
    throw new Error("云端凭证加密算法不受支持。");
  }
  const iterations = envelope.kdf.iterations;
  if (!Number.isInteger(iterations) || iterations < MIN_ITERATIONS || iterations > MAX_ITERATIONS) {
    throw new Error("云端凭证密钥派生迭代次数不合法。");
  }

  const salt = base64ToBytes(envelope.kdf.salt);
  const nonce = base64ToBytes(envelope.cipher.nonce);
  const combined = base64ToBytes(envelope.cipher.ciphertext);
  if (salt.length !== SALT_BYTES || nonce.length !== NONCE_BYTES || combined.length <= TAG_BYTES) {
    throw new Error("云端凭证密文结构损坏。");
  }

  const key = await deriveAesKey(passphrase, salt, iterations);
  let plaintextBuffer;
  try {
    plaintextBuffer = await crypto.subtle.decrypt(
      { name: "AES-GCM", iv: nonce, additionalData: buildAad(envelope.accountId, envelope.version), tagLength: 128 },
      key,
      combined
    );
  } catch {
    throw new Error("解密失败：同步加密口令不正确或云端文件已被篡改。");
  }

  const payload = JSON.parse(decoder.decode(plaintextBuffer));
  if (payload?.accountId !== envelope.accountId) {
    throw new Error("凭证载荷中的账号与信封声明不一致。");
  }
  if (typeof payload.token !== "string" || !payload.token.trim() || payload.token.length > MAX_TOKEN_LENGTH) {
    throw new Error("解密后的登录凭据无效。");
  }
  return {
    accountId: payload.accountId,
    token: payload.token,
    userInfoJson: typeof payload.userInfoJson === "string" ? payload.userInfoJson : "",
    savedAtUtc: payload.savedAtUtc || envelope.updatedAtUtc
  };
}

/** 编码为便携配置文本（与 PC 端 IMao 互通） */
export function encodeConfigBundle({ baseUrl, username, password, passphrase }) {
  const normalizedUrl = validateBaseUrl(baseUrl);
  const json = JSON.stringify({
    baseUrl: normalizedUrl,
    username: String(username || "").trim(),
    password: String(password || ""),
    passphrase: String(passphrase || "")
  });
  const b64 = bytesToBase64(encoder.encode(json)).replace(/=+$/, "").replace(/\+/g, "-").replace(/\//g, "_");
  return CONFIG_BUNDLE_PREFIX + b64;
}

/** 解码便携配置文本 */
export function decodeConfigBundle(text) {
  if (typeof text !== "string" || !text.trim()) throw new Error("配置文本为空。");
  const trimmed = text.trim();
  if (!trimmed.startsWith(CONFIG_BUNDLE_PREFIX)) {
    throw new Error(`配置文本格式不正确（应以 ${CONFIG_BUNDLE_PREFIX} 开头）。`);
  }
  let b64 = trimmed.slice(CONFIG_BUNDLE_PREFIX.length).replace(/-/g, "+").replace(/_/g, "/");
  const pad = b64.length % 4;
  if (pad === 2) b64 += "==";
  else if (pad === 3) b64 += "=";
  else if (pad !== 0) throw new Error("配置文本编码长度无效。");

  const parsed = JSON.parse(decoder.decode(base64ToBytes(b64)));
  const baseUrl = validateBaseUrl(parsed?.baseUrl);
  return {
    baseUrl,
    username: String(parsed?.username || "").trim(),
    password: String(parsed?.password || ""),
    passphrase: String(parsed?.passphrase || "")
  };
}
