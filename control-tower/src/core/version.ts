/**
 * One spelling of this build's version.
 *
 * Kept in source rather than read from package.json at runtime: the server
 * routes and the outbound user-agent both need it, a JSON import would tie two
 * server bundles to the packaging format, and this is a string that changes
 * once a release. Keep it in step with package.json when it moves.
 */
export const TOWER_VERSION = "0.1.0";

/**
 * How this tower introduces itself to the two public weather endpoints it
 * reads. A user-agent that names the project is the polite minimum for an
 * anonymous public API, and it must not name a person or a household.
 */
export const TOWER_USER_AGENT = `note4c-control-tower/${TOWER_VERSION}`;
