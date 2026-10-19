// Pure WebCrypto AWS SigV4 Presigner for Cloudflare R2 PUT URLs

export interface PresignOptions {
  accountId: string;
  bucket: string;
  key: string;
  accessKeyId: string;
  secretAccessKey: string;
  contentLength: number;
  contentType?: string;
  expiresInSeconds?: number; // Default: 900 (15 min)
  region?: string; // Default: "auto"
  now?: Date;
}

function rfc3986(str: string): string {
  return encodeURIComponent(str).replace(/[!'()*]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase()}`);
}

function pad2(n: number): string {
  return n < 10 ? `0${n}` : `${n}`;
}

function formatDate(date: Date): { dateShort: string; dateIso: string } {
  const y = date.getUTCFullYear();
  const m = pad2(date.getUTCMonth() + 1);
  const d = pad2(date.getUTCDate());
  const h = pad2(date.getUTCHours());
  const min = pad2(date.getUTCMinutes());
  const s = pad2(date.getUTCSeconds());
  return {
    dateShort: `${y}${m}${d}`,
    dateIso: `${y}${m}${d}T${h}${min}${s}Z`,
  };
}

async function hmac(key: ArrayBuffer | Uint8Array, data: string | Uint8Array): Promise<ArrayBuffer> {
  const cryptoKey = await crypto.subtle.importKey(
    "raw",
    key,
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"]
  );
  const dataBuf = typeof data === "string" ? new TextEncoder().encode(data) : data;
  return crypto.subtle.sign("HMAC", cryptoKey, dataBuf);
}

async function sha256Hex(data: string): Promise<string> {
  const buf = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(data));
  const arr = new Uint8Array(buf);
  let hex = "";
  for (let i = 0; i < arr.length; i++) {
    const b = arr[i].toString(16);
    hex += b.length === 1 ? `0${b}` : b;
  }
  return hex;
}

function bufToHex(buf: ArrayBuffer): string {
  const arr = new Uint8Array(buf);
  let hex = "";
  for (let i = 0; i < arr.length; i++) {
    const b = arr[i].toString(16);
    hex += b.length === 1 ? `0${b}` : b;
  }
  return hex;
}

export async function createPresignedPutUrl(opts: PresignOptions): Promise<string> {
  const region = opts.region || "auto";
  const now = opts.now || new Date();
  const expires = opts.expiresInSeconds ?? 900;
  const { dateShort, dateIso } = formatDate(now);
  const host = `${opts.accountId}.r2.cloudflarestorage.com`;

  // Path-style: /<bucket>/<key>
  const cleanKey = opts.key.startsWith("/") ? opts.key.slice(1) : opts.key;
  const canonicalUri = `/${rfc3986(opts.bucket)}/${cleanKey.split("/").map(rfc3986).join("/")}`;

  // Query parameters
  const credential = `${opts.accessKeyId}/${dateShort}/${region}/s3/aws4_request`;
  const queryParams: Record<string, string> = {
    "X-Amz-Algorithm": "AWS4-HMAC-SHA256",
    "X-Amz-Credential": credential,
    "X-Amz-Date": dateIso,
    "X-Amz-Expires": expires.toString(),
    "X-Amz-SignedHeaders": "content-length;host",
  };

  const canonicalQuery = Object.keys(queryParams)
    .sort()
    .map((k) => `${rfc3986(k)}=${rfc3986(queryParams[k])}`)
    .join("&");

  // Canonical headers (sorted by lowercase name, trimmed, each with trailing newline)
  const canonicalHeaders = `content-length:${opts.contentLength}\nhost:${host}\n`;

  // Payload hash for presigned URL is always UNSIGNED-PAYLOAD
  const payloadHash = "UNSIGNED-PAYLOAD";

  const canonicalRequest = [
    "PUT",
    canonicalUri,
    canonicalQuery,
    canonicalHeaders,
    "content-length;host",
    payloadHash,
  ].join("\n");

  const hashedCanonical = await sha256Hex(canonicalRequest);

  const stringToSign = [
    "AWS4-HMAC-SHA256",
    dateIso,
    `${dateShort}/${region}/s3/aws4_request`,
    hashedCanonical,
  ].join("\n");

  // Derived signing key
  const kDate = await hmac(new TextEncoder().encode(`AWS4${opts.secretAccessKey}`), dateShort);
  const kRegion = await hmac(kDate, region);
  const kService = await hmac(kRegion, "s3");
  const kSigning = await hmac(kService, "aws4_request");

  const signatureBuf = await hmac(kSigning, stringToSign);
  const signature = bufToHex(signatureBuf);

  return `https://${host}${canonicalUri}?${canonicalQuery}&X-Amz-Signature=${signature}`;
}
