"use client";

import { useEffect, useState } from "react";

/**
 * A media query as state, without a hydration mismatch.
 *
 * It reports `false` on the server and on the first client render, then the
 * real answer immediately after mount. Every caller here is deciding between
 * two *layouts of the same content*, so one frame of the wide layout on a
 * phone is a reflow, not a flash of something wrong — whereas reading
 * `window.innerWidth` during render is a hydration error, and that is how the
 * designer ended up with a zoom that only corrected itself on the second
 * render anyway.
 */
export function useMediaQuery(query: string): boolean {
  const [matches, setMatches] = useState(false);

  useEffect(() => {
    const list = window.matchMedia(query);
    setMatches(list.matches);
    const onChange = (event: MediaQueryListEvent) => setMatches(event.matches);
    list.addEventListener("change", onChange);
    return () => list.removeEventListener("change", onChange);
  }, [query]);

  return matches;
}

/** The breakpoint where the rail gives way to the bottom tabs. */
export const NARROW_QUERY = "(max-width: 760px)";

/** The breakpoint where the designer loses its right-hand column. */
export const SINGLE_COLUMN_QUERY = "(max-width: 900px)";

export function useIsNarrow(): boolean {
  return useMediaQuery(NARROW_QUERY);
}
