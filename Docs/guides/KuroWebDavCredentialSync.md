# WebDAV 库街区登录凭证同步规范（v1）

本规范定义 PC 端 IMao、浏览器扩展 `KuroMapSync` 以及后续移动端（`IMAO-Pocket`）共用的 WebDAV 库街区登录凭证同步协议。

- **职责边界**：点位进度继续由既有库街区双向同步负责；WebDAV **仅同步库街区登录凭证**，且为**默认关闭的可选项**。
- **三端对等**：PC IMao、浏览器扩展、移动端各自独立直连玩家自己的 WebDAV，互不依赖。

## 1. 目录与多账号命名

- 同步目录：`<WebDAV BaseUrl 规范化末尾带 />imao-kuro-sync/v1/`
- 单账号文件：`kuro_<accountId>.json`（`accountId` 为 1–24 位纯数字库街区账号 ID）。
- 仅使用标准 HTTP 子集：`MKCOL`、`PROPFIND (Depth: 1)`、`GET`、`PUT`、`DELETE`，认证使用 HTTP Basic Auth。
- 公网地址强制要求 `https://`，仅本机回环或局域网私有 IP 允许 `http://`。

## 2. 端到端加密信封格式

- **密钥派生**：`PBKDF2-HMAC-SHA256`，默认 `210,000` 次迭代（允许范围 `100,000–1,000,000`），16 字节随机 Salt，输出 32 字节 AES-256 密钥。
- **对称加密**：`AES-256-GCM`，12 字节随机 Nonce，16 字节认证标签；密文字段为 `Base64(ciphertext || tag)`。
- **AAD（附加认证数据）**：UTF-8 字节串 `imao-kuro-webdav-v1|<accountId>|<version>`。
- **版本防覆盖**：上传前先 `GET` 读取远端当前 `version`（不存在视为 0），再以 `version + 1` 加密 `PUT`。

```json
{
  "schemaVersion": 1,
  "accountId": "10146974",
  "version": 3,
  "updatedAtUtc": "2026-11-22T09:30:00.000Z",
  "sourceDevice": "browser",
  "displayName": "大号昵称",
  "kdf": {
    "alg": "PBKDF2-SHA256",
    "iterations": 210000,
    "salt": "<Base64 16B>"
  },
  "cipher": {
    "alg": "AES-256-GCM",
    "nonce": "<Base64 12B>",
    "ciphertext": "<Base64 ciphertext||tag(16B)>"
  }
}
```

密文明文载荷（UTF-8 JSON）：

```json
{
  "accountId": "10146974",
  "token": "<AKI_MAP_USER_TOKEN>",
  "userInfoJson": "<可选：AKI_MAP_USER_INFO 原始 JSON>",
  "savedAtUtc": "2026-11-22T09:30:00.000Z"
}
```

## 3. 便携配置文本

用于 PC IMao 与浏览器扩展之间一键复制/粘贴配置：

```text
imao-webdav-v1:<base64url(JSON({"baseUrl","username","password","passphrase"}))>
```
