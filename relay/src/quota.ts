// Quota calculation and blob pruning math for PickleLauncher Shared Packs Relay

export interface BlobUploadRequest {
  sha512: string;
  size: number;
}

export interface QuotaCheckResult {
  ok: boolean;
  error?: string;
}

export function checkUploadQuota(
  publisherQuotaBytes: number,
  publisherMaxFileBytes: number,
  currentStoredBytes: number,
  missingBlobs: BlobUploadRequest[]
): QuotaCheckResult {
  let newBytes = 0;
  for (const blob of missingBlobs) {
    if (blob.size < 0) {
      return { ok: false, error: "File size cannot be negative" };
    }
    if (blob.size > publisherMaxFileBytes) {
      return {
        ok: false,
        error: `File ${blob.sha512.slice(0, 16)}... exceeds maximum file size limit (${blob.size} > ${publisherMaxFileBytes})`,
      };
    }
    newBytes += blob.size;
  }

  if (currentStoredBytes + newBytes > publisherQuotaBytes) {
    return {
      ok: false,
      error: `Publisher quota exceeded: uploading ${newBytes} bytes would reach ${currentStoredBytes + newBytes} bytes (quota is ${publisherQuotaBytes} bytes)`,
    };
  }

  return { ok: true };
}

/**
 * Given the blobs referenced in the recent revisions (e.g. N, N-1, N-2)
 * and blobs referenced by an older revision being retired (e.g. N-3),
 * determines which blobs are no longer referenced by any recent revision.
 */
export function findOrphanedBlobs(
  retainedBlobs: Set<string>,
  retiredRevisionBlobs: string[]
): string[] {
  const orphans = new Set<string>();
  for (const sha of retiredRevisionBlobs) {
    if (!retainedBlobs.has(sha)) {
      orphans.add(sha);
    }
  }
  return Array.from(orphans);
}
