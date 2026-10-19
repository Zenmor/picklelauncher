import { isAllowedModrinthUrl, isSafeManifestPath, isValidPackId, isValidRevision, isValidSha512 } from "./validation.ts";

const MAX_BYTES = 8 * 1024 * 1024;
const MAX_FILES = 5000;
const MAX_FILE_BYTES = 512 * 1024 * 1024;
const MAX_TOTAL_BYTES = 4 * 1024 * 1024 * 1024;
const MAX_NAME_CHARS = 64;
const MAX_CHANGELOG_CHARS = 20000;
const LOADERS = new Set(["net.fabricmc.fabric-loader", "org.quiltmc.quilt-loader", "net.minecraftforge", "net.neoforged"]);

export interface ManifestFile {
  path: string;
  size: number;
  sha512: string;
  urls: string[];
}

export interface PackManifest {
  formatVersion: number;
  packId: string;
  name: string;
  revision: number;
  files: ManifestFile[];
}

export interface ParsedManifest {
  manifest: PackManifest;
  requiredBlobs: Map<string, number>;
}

function isPlainText(value: unknown, maxLength: number): value is string {
  return (
    typeof value === "string" &&
    value.length > 0 &&
    value.length <= maxLength &&
    value.trim() === value &&
    !/[\x00-\x1f\x7f\u2028\u2029\u202a-\u202e\u2066-\u2069]/.test(value)
  );
}

export function parsePackManifest(bytes: Uint8Array, expectedPackId: string, expectedRevision: number): ParsedManifest | null {
  if (bytes.byteLength > MAX_BYTES) return null;

  let value: unknown;
  try {
    value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  } catch {
    return null;
  }
  if (typeof value !== "object" || value === null || Array.isArray(value)) return null;

  const root = value as Record<string, unknown>;
  if (
    root.formatVersion !== 1 ||
    root.packId !== expectedPackId ||
    !isValidPackId(root.packId) ||
    root.revision !== expectedRevision ||
    !isValidRevision(root.revision) ||
    !isPlainText(root.name, MAX_NAME_CHARS) ||
    !Array.isArray(root.components) ||
    root.components.length > 8 ||
    !Array.isArray(root.files) ||
    root.files.length > MAX_FILES
  ) {
    return null;
  }
  if (root.changelog !== undefined &&
      (typeof root.changelog !== "string" || root.changelog.length > MAX_CHANGELOG_CHARS ||
       /[\x00-\x08\x0b\x0c\x0e-\x1f\x7f\u2028\u2029\u202a-\u202e\u2066-\u2069]/.test(root.changelog))) {
    return null;
  }

  const componentIds = new Set<string>();
  let loaderCount = 0;
  let hasMinecraft = false;
  for (const item of root.components) {
    if (typeof item !== "object" || item === null || Array.isArray(item)) return null;
    const component = item as Record<string, unknown>;
    if (
      typeof component.uid !== "string" ||
      component.uid.length < 1 ||
      component.uid.length > 64 ||
      !isPlainText(component.version, 64)
    ) {
      return null;
    }
    if (component.uid === "net.minecraft") hasMinecraft = true;
    else if (LOADERS.has(component.uid)) loaderCount++;
    else return null;
    if (componentIds.has(component.uid)) return null;
    componentIds.add(component.uid);
  }
  if (!hasMinecraft || loaderCount > 1) return null;

  const files: ManifestFile[] = [];
  const foldedPaths = new Set<string>();
  const fileAndDirectoryPaths = new Set<string>();
  const requiredBlobs = new Map<string, number>();
  let totalBytes = 0;

  for (const item of root.files) {
    if (typeof item !== "object" || item === null || Array.isArray(item)) return null;
    const file = item as Record<string, unknown>;
    if (
      typeof file.path !== "string" ||
      !isSafeManifestPath(file.path) ||
      typeof file.size !== "number" ||
      !Number.isSafeInteger(file.size) ||
      file.size < 0 ||
      file.size > MAX_FILE_BYTES ||
      typeof file.sha512 !== "string" ||
      !isValidSha512(file.sha512)
    ) {
      return null;
    }
    const folded = file.path.toLowerCase();
    if (foldedPaths.has(folded)) return null;
    foldedPaths.add(folded);
    for (let slash = folded.indexOf("/"); slash !== -1; slash = folded.indexOf("/", slash + 1)) {
      fileAndDirectoryPaths.add(folded.slice(0, slash));
    }
    totalBytes += file.size;
    if (totalBytes > MAX_TOTAL_BYTES) return null;

    let urls: string[] = [];
    if (file.urls !== undefined) {
      if (!Array.isArray(file.urls) || file.urls.length > 4) return null;
      for (const url of file.urls) {
        if (typeof url !== "string" || !isAllowedModrinthUrl(url)) return null;
      }
      urls = [...new Set(file.urls as string[])];
    }
    if (urls.length === 0) {
      const priorSize = requiredBlobs.get(file.sha512);
      if (priorSize !== undefined && priorSize !== file.size) return null;
      requiredBlobs.set(file.sha512, file.size);
    }
    files.push({ path: file.path, size: file.size, sha512: file.sha512, urls });
  }

  for (const path of foldedPaths) {
    if (fileAndDirectoryPaths.has(path)) return null;
  }

  return {
    manifest: {
      formatVersion: 1,
      packId: root.packId,
      name: root.name,
      revision: root.revision,
      files,
    },
    requiredBlobs,
  };
}
