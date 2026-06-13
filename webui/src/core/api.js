/**
 * API helpers — talk dirty to the hardware via REST. Every endpoint expects
 * JSON in/out unless noted; the ESP32-S3 handler core (Core 0) picks these up
 * from httpTask and routes them to the right doms. :3
 */

/**
 * POST a JSON body to a firmware endpoint. Returns the raw fetch Response
 * (or null on network error). Callers should .json() it themselves.
 */
export async function post(url, body = {}) {
  try {
    return await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
  } catch (e) {
    return null;
  }
}

/**
 * GET a firmware endpoint, returning the parsed JSON (or null).
 */
export async function get(url) {
  try {
    const r = await fetch(url);
    return await r.json();
  } catch (e) {
    return null;
  }
}

/**
 * GET raw text from a firmware endpoint (for /api/log).
 */
export async function getText(url) {
  try {
    const r = await fetch(url);
    return await r.text();
  } catch (e) {
    return null;
  }
}