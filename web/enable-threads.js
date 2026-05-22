// NOTE: This file creates a service worker that cross-origin-isolates the page (read more here: https://web.dev/coop-coep/) which allows us to use wasm threads.
// Normally you would set the COOP and COEP headers on the server to do this, but Github Pages doesn't allow this, so this is a hack to do that.

/* Edited version of: coi-serviceworker v0.1.6 - Guido Zuidhof, licensed under MIT */
// From here: https://github.com/gzuidhof/coi-serviceworker
//
// Hardened: a failed re-fetch no longer crashes the worker (the original read
// `.status` off an undefined result, which broke the whole page load).
// Requests that cannot be re-fetched as-is - notably navigation requests,
// which stricter browsers refuse to let a worker re-issue - are retried by
// URL, and a genuine failure surfaces as a clean network error.
if(typeof window === 'undefined') {
  self.addEventListener("install", () => self.skipWaiting());
  self.addEventListener("activate", e => e.waitUntil(self.clients.claim()));

  async function handleFetch(request) {
    if(request.mode === "no-cors") { // We need to set `credentials` to "omit" for no-cors requests, per this comment: https://bugs.chromium.org/p/chromium/issues/detail?id=1309901#c7
      request = new Request(request.url, {
        cache: request.cache,
        credentials: "omit",
        headers: request.headers,
        integrity: request.integrity,
        destination: request.destination,
        keepalive: request.keepalive,
        method: request.method,
        mode: request.mode,
        redirect: request.redirect,
        referrer: request.referrer,
        referrerPolicy: request.referrerPolicy,
        signal: request.signal,
      });
    }

    let r;
    try {
      r = await fetch(request);
    } catch (e) {
      // Some requests (e.g. navigations on stricter browsers) cannot be
      // re-fetched as-is; retry by URL, and fail cleanly if that fails too.
      try {
        r = await fetch(request.url);
      } catch (e2) {
        console.error("COOP/COEP Service Worker fetch failed:", e2);
        return Response.error();
      }
    }
    if(!r || r.status === 0) {
      return r;
    }

    const headers = new Headers(r.headers);
    headers.set("Cross-Origin-Embedder-Policy", "credentialless"); // or: require-corp
    headers.set("Cross-Origin-Opener-Policy", "same-origin");

    return new Response(r.body, { status: r.status, statusText: r.statusText, headers });
  }

  self.addEventListener("fetch", function(e) {
    const request = e.request;
    // Requests we can't safely re-serve are left to the browser's default path.
    if(request.cache === "only-if-cached" && request.mode !== "same-origin") {
      return;
    }
    e.respondWith(handleFetch(request)); // respondWith must be executed synchonously (but can be passed a Promise)
  });

} else {
  (async function() {
    if(window.crossOriginIsolated !== false) return;

    let registration = await navigator.serviceWorker.register(window.document.currentScript.src).catch(e => console.error("COOP/COEP Service Worker failed to register:", e));
    if(registration) {
      console.log("COOP/COEP Service Worker registered", registration.scope);

      registration.addEventListener("updatefound", () => {
        console.log("Reloading page to make use of updated COOP/COEP Service Worker.");
        window.location.reload();
      });

      // If the registration is active, but it's not controlling the page
      if(registration.active && !navigator.serviceWorker.controller) {
        console.log("Reloading page to make use of COOP/COEP Service Worker.");
        window.location.reload();
      }
    }
  })();
}

// Code to deregister:
// let registrations = await navigator.serviceWorker.getRegistrations();
// for(let registration of registrations) {
//   await registration.unregister();
// }
