// Cross-origin isolation for wrap.html on hosts that cannot send COOP/COEP
// (GitHub Pages). Installed at the runtime site's root (next to runtime/ and
// boxedwine/, whose frame the runtime page opens). wrap.js registers it there
// when the page is not isolated, then reloads; from then on it stamps the
// headers on the site's pages and on everything they fetch from this origin.
//
// Inside a host the runtime page is usually isolated already, by the host's
// own worker (buzz-remote: sw-apps-coi.js at the apps root). Once registered,
// this narrower worker takes over the site, with the same headers.
const COI_HEADERS = {
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Opener-Policy": "same-origin",
};
const NULL_BODY_STATUSES = new Set([101, 204, 205, 304]);

function shouldIsolate(request) {
  if (request.mode === "navigate") return true;
  try {
    return new URL(request.url).origin === self.location.origin;
  } catch {
    return false;
  }
}

function withCoi(response) {
  if (!response || response.type === "opaque" || response.type === "opaqueredirect") return response;
  if (response.status < 200 || response.status > 599) return response;
  try {
    const headers = new Headers(response.headers);
    for (const [name, value] of Object.entries(COI_HEADERS)) headers.set(name, value);
    return new Response(NULL_BODY_STATUSES.has(response.status) ? null : response.body, {
      status: response.status,
      statusText: response.statusText,
      headers,
    });
  } catch {
    return response;
  }
}

self.addEventListener("install", (event) => event.waitUntil(self.skipWaiting()));
self.addEventListener("activate", (event) => event.waitUntil(self.clients.claim()));
self.addEventListener("fetch", (event) => {
  if (!shouldIsolate(event.request)) return;
  event.respondWith(fetch(event.request).then(withCoi).catch(() => fetch(event.request)));
});
