/**
 * Login rate limit: five attempts per minute, per client.
 *
 * In memory on purpose. The tower is a single process on one Mac, and a
 * limiter that survives a restart would need durable state for a control that
 * only has to slow down an online guesser.
 */

export const LOGIN_ATTEMPT_LIMIT = 5;
export const LOGIN_WINDOW_MS = 60_000;

const attempts = new Map<string, number[]>();

export interface RateLimitVerdict {
  allowed: boolean;
  remaining: number;
  retryAfterSeconds: number;
}

export function checkLoginRate(
  key: string,
  now: number = Date.now(),
): RateLimitVerdict {
  const recent = (attempts.get(key) ?? []).filter(
    (at) => now - at < LOGIN_WINDOW_MS,
  );
  attempts.set(key, recent);

  if (recent.length >= LOGIN_ATTEMPT_LIMIT) {
    const oldest = recent[0] as number;
    return {
      allowed: false,
      remaining: 0,
      retryAfterSeconds: Math.max(
        1,
        Math.ceil((LOGIN_WINDOW_MS - (now - oldest)) / 1000),
      ),
    };
  }

  return {
    allowed: true,
    remaining: LOGIN_ATTEMPT_LIMIT - recent.length,
    retryAfterSeconds: 0,
  };
}

/** Only failures count, so a working passphrase is never rate limited. */
export function recordFailedLogin(key: string, now: number = Date.now()): void {
  const recent = (attempts.get(key) ?? []).filter(
    (at) => now - at < LOGIN_WINDOW_MS,
  );
  recent.push(now);
  attempts.set(key, recent);
}

export function clearLoginRate(key?: string): void {
  if (key === undefined) attempts.clear();
  else attempts.delete(key);
}
