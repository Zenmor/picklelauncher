// Path and parameter validation for PickleLauncher Shared Packs Relay

const HEX_LOWER = /^[0-9a-f]+$/;
const TOKEN_CHARS = /^[A-Za-z0-9_-]+$/;
const SEGMENT_CHARS = /^[A-Za-z0-9_.-]+$/;

export function isValidSha512(hash: string): boolean {
  return typeof hash === "string" && hash.length === 128 && HEX_LOWER.test(hash);
}

export function isValidSha256(hash: string): boolean {
  return typeof hash === "string" && hash.length === 64 && HEX_LOWER.test(hash);
}

export function isValidPackId(id: string): boolean {
  return typeof id === "string" && id.length >= 1 && id.length <= 64 && TOKEN_CHARS.test(id);
}

export function isValidToken(token: string): boolean {
  return typeof token === "string" && token.length >= 22 && token.length <= 128 && TOKEN_CHARS.test(token);
}

export function isValidPrefix(prefix: string): boolean {
  if (prefix === "") return true;
  if (typeof prefix !== "string" || prefix.length > 128) return false;
  if (prefix.startsWith("/") || prefix.endsWith("/")) return false;

  const segments = prefix.split("/");
  for (const seg of segments) {
    if (!seg || seg === "." || seg === ".." || !SEGMENT_CHARS.test(seg)) {
      return false;
    }
  }
  return true;
}

export function isValidKey(key: string): boolean {
  if (typeof key !== "string" || key.length === 0 || key.length > 512) return false;
  if (key.startsWith("/") || key.endsWith("/")) return false;

  const segments = key.split("/");
  for (const seg of segments) {
    if (!seg || seg === "." || seg === ".." || !SEGMENT_CHARS.test(seg)) {
      return false;
    }
  }
  return true;
}

export function isValidRevision(rev: number): boolean {
  return typeof rev === "number" && Number.isInteger(rev) && rev >= 1 && rev <= 2147483647;
}

export function buildBlobKey(sha512: string, token: string, prefix?: string): string {
  if (!isValidSha512(sha512)) throw new Error("Invalid sha512");
  if (!isValidToken(token)) throw new Error("Invalid token");
  const base = `${token}/files/${sha512}`;
  if (prefix && prefix !== "") {
    if (!isValidPrefix(prefix)) throw new Error("Invalid prefix");
    return `${prefix}/${base}`;
  }
  return base;
}

// Common key prefix of every blob of a pack, for listing. buildBlobKey(sha512, ...) === buildBlobPrefix(...) + sha512.
export function buildBlobPrefix(token: string, prefix?: string): string {
  if (!isValidToken(token)) throw new Error("Invalid token");
  const base = `${token}/files/`;
  if (prefix && prefix !== "") {
    if (!isValidPrefix(prefix)) throw new Error("Invalid prefix");
    return `${prefix}/${base}`;
  }
  return base;
}

export function buildRevisionKey(token: string, rev: number, prefix?: string): string {
  if (!isValidToken(token)) throw new Error("Invalid token");
  if (!isValidRevision(rev)) throw new Error("Invalid revision");
  const base = `${token}/revisions/${rev}.json`;
  if (prefix && prefix !== "") {
    if (!isValidPrefix(prefix)) throw new Error("Invalid prefix");
    return `${prefix}/${base}`;
  }
  return base;
}

export function buildLatestKey(token: string, prefix?: string): string {
  if (!isValidToken(token)) throw new Error("Invalid token");
  const base = `${token}/latest.json`;
  if (prefix && prefix !== "") {
    if (!isValidPrefix(prefix)) throw new Error("Invalid prefix");
    return `${prefix}/${base}`;
  }
  return base;
}

export function isSafeManifestPath(path: string): boolean {
  if (typeof path !== "string" || path.length < 1 || path.length > 240 || path.toLowerCase().endsWith(".packnew")) return false;
  const segments = path.split("/");
  if (segments.some((segment) => {
    if (
      segment.length < 1 ||
      segment.length > 100 ||
      segment === "." ||
      segment === ".." ||
      segment.endsWith(".") ||
      segment.endsWith(" ") ||
      /[<>:"|?*\\\x00-\x1f\x7f]/.test(segment)
    ) {
      return true;
    }
    const stem = segment.split(".")[0].trim().toUpperCase();
    return (
      ["CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"].includes(stem) ||
      /^(COM|LPT)([1-9¹²³])$/.test(stem)
    );
  })) {
    return false;
  }
  const lower = path.toLowerCase();
  const top = segments[0].toLowerCase();
  return !["saves", "screenshots", "logs", "crash-reports"].includes(top) &&
    !lower.includes("/.index/");
}

export function isAllowedModrinthUrl(value: string): boolean {
  try {
    const url = new URL(value);
    return (
      value.length <= 2048 &&
      url.protocol === "https:" &&
      url.hostname.toLowerCase() === "cdn.modrinth.com" &&
      (url.port === "" || url.port === "443") &&
      url.username === "" &&
      url.password === "" &&
      url.hash === "" &&
      url.pathname.startsWith("/data/")
    );
  } catch {
    return false;
  }
}
