// Authentication, Token and Code helpers for PickleLauncher Shared Packs Relay

export interface Publisher {
  id: string;
  label: string;
  token_hash: string;
  quota_bytes: number;
  max_file_bytes: number;
  max_packs: number;
  rate_window_start: number;
  request_count: number;
  created_at: number;
  revoked_at: number | null;
}

export interface Pack {
  pack_id: string;
  publisher_id: string;
  pack_secret_hash: string;
  read_token: string;
  storage_token: string;
  prefix: string;
  current_rev: number;
  stored_bytes: number;
  created_at: number;
}

export interface PublisherCodeData {
  worker: string;
  code: string;
  label: string;
  exp: number; // unix timestamp ms
}

export async function sha256Hex(text: string): Promise<string> {
  const buf = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  const arr = new Uint8Array(buf);
  let hex = "";
  for (let i = 0; i < arr.length; i++) {
    const b = arr[i].toString(16);
    hex += b.length === 1 ? `0${b}` : b;
  }
  return hex;
}

export function generateRandomId(prefix = "pub", byteLength = 16): string {
  const bytes = new Uint8Array(byteLength);
  crypto.getRandomValues(bytes);
  return `${prefix}_${bytesToBase64Url(bytes)}`;
}

export function generateRandomSecret(byteLength = 32): string {
  const bytes = new Uint8Array(byteLength);
  crypto.getRandomValues(bytes);
  return bytesToBase64Url(bytes);
}

export function bytesToBase64Url(bytes: Uint8Array): string {
  let binary = "";
  for (let i = 0; i < bytes.length; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function base64UrlToBytes(str: string): Uint8Array {
  let base64 = str.replace(/-/g, "+").replace(/_/g, "/");
  while (base64.length % 4 !== 0) {
    base64 += "=";
  }
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

const PUB_SCHEME = "picklepub:1:";

export function encodePublisherCode(data: PublisherCodeData): string {
  const json = JSON.stringify(data);
  const bytes = new TextEncoder().encode(json);
  return `${PUB_SCHEME}${bytesToBase64Url(bytes)}`;
}

export function parsePublisherCode(codeStr: string): PublisherCodeData | null {
  const trimmed = codeStr.trim();
  if (!trimmed.startsWith(PUB_SCHEME)) return null;

  try {
    const b64 = trimmed.slice(PUB_SCHEME.length);
    const bytes = base64UrlToBytes(b64);
    const json = new TextDecoder().decode(bytes);
    const data = JSON.parse(json);
    if (!data.worker || !data.code || !data.label || !data.exp) return null;
    return data as PublisherCodeData;
  } catch {
    return null;
  }
}
