/**
 * Outbound HTTP for source adapters. Deliberately blunt: a fixed timeout, no
 * redirects, no proxies, no retries. Every caller here reads a source the
 * tower does not own, so a slow or hostile answer must never hold a request.
 */

export class SourceError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "SourceError";
  }
}

export interface FetchJsonOptions {
  timeoutMs?: number;
  headers?: Record<string, string>;
}

export async function fetchJson<T = unknown>(
  url: string,
  options: FetchJsonOptions = {},
): Promise<T> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), options.timeoutMs ?? 20_000);
  try {
    const response = await fetch(url, {
      method: "GET",
      redirect: "error",
      signal: controller.signal,
      headers: { accept: "application/json", ...options.headers },
      cache: "no-store",
    });
    if (!response.ok) {
      // Status only. A body from an unowned source can carry a URL or a token
      // echoed back, and this string reaches Diagnostics.
      throw new SourceError(`HTTP ${response.status}`);
    }
    return (await response.json()) as T;
  } catch (error) {
    if (error instanceof SourceError) throw error;
    if (error instanceof Error && error.name === "AbortError") {
      throw new SourceError("Timed out");
    }
    // Never surface the raw error: fetch failure messages embed the full URL.
    throw new SourceError(
      error instanceof Error ? error.constructor.name : "Request failed",
    );
  } finally {
    clearTimeout(timer);
  }
}
