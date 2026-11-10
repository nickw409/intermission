package httpx

import (
	"net/http"
	"strings"

	"github.com/nickw409/intermission/api/internal/apperr"
)

// CSRF defence (spec: "CSRF"), applied in Wrap to every request whose method
// is not GET, HEAD or OPTIONS, so no route can forget it. Three independent
// checks, all of which must pass:
//
//  1. net/http.CrossOriginProtection: rejects requests a browser marks as
//     cross-origin (Sec-Fetch-Site), and, for older browsers, an Origin whose
//     host differs from the request's Host.
//  2. Origin must be present and equal to the configured site origin
//     (RECS_SITE_ORIGIN). A missing Origin is rejected (CrossOriginProtection
//     alone would allow it, to support non-browser clients).
//  3. X-Requested-With must be exactly "recs". A cross-site form can't set a
//     custom header, and a cross-site fetch that sets one needs a CORS
//     preflight, which recs-api never grants.
//
// No trusted origins or bypass patterns are configured on
// CrossOriginProtection: the reverse proxy preserves Host, so the site's own
// requests pass check 1 on their own.

const csrfMessage = "missing or invalid X-Requested-With or Origin"

// CSRF returns middleware enforcing the checks above. With an empty
// siteOrigin every mutating request is rejected.
func CSRF(siteOrigin string) func(http.Handler) http.Handler {
	cop := http.NewCrossOriginProtection()
	siteOrigin = strings.ToLower(siteOrigin)
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if !isSafeMethod(r.Method) && !csrfOK(cop, siteOrigin, r) {
				apperr.WriteError(w, http.StatusForbidden, apperr.CodeCSRFRejected, csrfMessage)
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

func csrfOK(cop *http.CrossOriginProtection, siteOrigin string, r *http.Request) bool {
	if siteOrigin == "" {
		return false
	}
	if err := cop.Check(r); err != nil {
		return false
	}
	origin := r.Header.Values("Origin")
	if len(origin) != 1 || strings.ToLower(origin[0]) != siteOrigin {
		return false
	}
	xrw := r.Header.Values("X-Requested-With")
	return len(xrw) == 1 && xrw[0] == "recs"
}
