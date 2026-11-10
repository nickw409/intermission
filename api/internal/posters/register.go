package posters

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/moviecache"
	"github.com/nickw409/intermission/api/internal/ratelimit"
	"github.com/nickw409/intermission/api/internal/reqid"
)

// How a poster is served (spec: "Posters"):
//
//  1. The movie ID is parsed (digits only) and the disk cache checked.
//     IDs in the short-lived negative cache (no poster to be had: unknown
//     movie, no TMDB id, no TMDB poster, a failed fetch) stop here too.
//  2. A token is taken from a global lookup limiter, then the TMDB ID comes
//     from the movie cache (moviecache; recs-model fills misses). The
//     limiter comes first so a loop over random IDs can't turn into a flood
//     of recs-model calls.
//  3. One token is taken from a global upstream limiter, so a crawler can't
//     drain the TMDB quota or churn the disk cache.
//  4. TMDB's API gives the poster path for the movie; the path must look
//     like "/<letters, digits, _ or ->.jpg".
//  5. The w342 image is fetched from TMDB's image host, size-capped and
//     checked to be a JPEG, stored on disk and served.
//
// Any failure at any step serves a fixed SVG placeholder with 200; this
// route never answers 5xx. Only the two TMDB hosts are ever contacted, a
// redirect may not leave the host it started on, and the API key is sent
// only to the API host. Errors are logged without the request URL (it
// carries the key) and with the key scrubbed as a second line of defence.

// Limits and endpoints.
const (
	// MaxImageBytes caps one poster image (w342 JPEGs are ~20-80 KB).
	MaxImageBytes = 1 << 20
	// maxAPIBytes caps one TMDB API response (movie details are ~10 KB).
	maxAPIBytes = 256 << 10
	// upstreamTimeout bounds the whole TMDB exchange for one poster.
	upstreamTimeout = 5 * time.Second
	maxRedirects    = 3

	defaultAPIBase   = "https://api.themoviedb.org"
	defaultImageBase = "https://image.tmdb.org"
)

// UpstreamLimit is the global TMDB budget: one poster fetch (two TMDB
// requests) every 100 ms on average, bursts of 20. TMDB allows roughly 40
// requests per second.
var UpstreamLimit = ratelimit.Config{Every: 100 * time.Millisecond, Burst: 20}

// LookupLimit is the global budget for poster requests that miss both disk
// caches and need a movie lookup: 40 per second, bursts of 100 (a page of
// posters nobody has viewed yet).
var LookupLimit = ratelimit.Config{Every: 25 * time.Millisecond, Burst: 100}

// Negative cache: how long a movie without a poster is remembered, and how
// many such movies at most.
const (
	negativeTTL     = 10 * time.Minute
	negativeEntries = 50_000
)

// placeholderSVG is served whenever a real poster can't be (contracts/
// fixtures/public/getPoster.json, ok_placeholder).
const placeholderSVG = `<svg xmlns="http://www.w3.org/2000/svg" width="342" height="513" viewBox="0 0 342 513"><rect width="342" height="513" fill="#2b2b2b"/></svg>`

// posterPath is what TMDB poster_path values look like.
var posterPath = regexp.MustCompile(`^/[A-Za-z0-9_-]{1,64}\.jpg$`)

// Register adds this package's routes to mux. Called once from main.go.
//
//	GET /api/posters/{movieId}
func Register(mux *http.ServeMux, d *app.Deps) {
	register(mux, d, options{})
}

// options are the parts tests replace.
type options struct {
	apiBase   string            // default defaultAPIBase
	imageBase string            // default defaultImageBase
	limiter   *ratelimit.Global // upstream (TMDB); default UpstreamLimit
	lookup    *ratelimit.Global // movie lookups; default LookupLimit
	timeout   time.Duration
	now       func() time.Time // negative-cache clock
}

type handler struct {
	movies    *moviecache.Cache
	logger    *slog.Logger
	key       app.Secret
	apiBase   *url.URL
	imageBase *url.URL
	client    *http.Client
	limiter   *ratelimit.Global
	lookup    *ratelimit.Global
	timeout   time.Duration
	cache     *diskCache
	negative  *negCache
}

func register(mux *http.ServeMux, d *app.Deps, o options) {
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	if o.apiBase == "" {
		o.apiBase = defaultAPIBase
	}
	if o.imageBase == "" {
		o.imageBase = defaultImageBase
	}
	if o.limiter == nil {
		o.limiter = ratelimit.NewGlobal(UpstreamLimit)
	}
	if o.lookup == nil {
		o.lookup = ratelimit.NewGlobal(LookupLimit)
	}
	if o.timeout <= 0 {
		o.timeout = upstreamTimeout
	}
	apiBase, err1 := url.Parse(o.apiBase)
	imageBase, err2 := url.Parse(o.imageBase)
	if err := errors.Join(err1, err2); err != nil {
		panic(fmt.Sprintf("posters: bad TMDB base URL: %v", err))
	}
	h := &handler{
		movies:    d.Movies,
		logger:    logger,
		key:       d.Config.TMDBAPIKey,
		apiBase:   apiBase,
		imageBase: imageBase,
		limiter:   o.limiter,
		lookup:    o.lookup,
		timeout:   o.timeout,
		cache:     newDiskCache(d.Config.PosterCacheDir, d.Config.PosterCacheMaxByte, logger),
		negative:  newNegCache(negativeEntries, negativeTTL, o.now),
	}
	// No proxy from the environment: requests go straight to the TMDB hosts.
	transport := http.DefaultTransport.(*http.Transport).Clone()
	transport.Proxy = nil
	h.client = &http.Client{
		Transport:     transport,
		Timeout:       o.timeout,
		CheckRedirect: h.checkRedirect,
	}
	mux.HandleFunc("GET /api/posters/{movieId}", h.serve)
}

func (h *handler) serve(w http.ResponseWriter, r *http.Request) {
	id, err := moviecache.ParseMovieID(r.PathValue("movieId"))
	if err != nil {
		apperr.Write(w, err)
		return
	}
	if img, ok := h.cache.get(id); ok {
		writeJPEG(w, img)
		return
	}
	if h.key == "" || h.negative.has(id) {
		writePlaceholder(w)
		return
	}
	img, err := h.fetch(r.Context(), id)
	if err != nil {
		var tr transient
		switch {
		case errors.Is(err, errNoPoster):
			h.negative.add(id)
		case errors.Is(err, errLimited):
		case errors.As(err, &tr):
			h.logger.WarnContext(r.Context(), "posters: movie lookup failed", "request_id", reqid.FromContext(r.Context()),
				"movie_id", id, "err", h.scrub(err))
		default:
			h.negative.add(id)
			h.logger.WarnContext(r.Context(), "posters: fetch failed", "request_id", reqid.FromContext(r.Context()),
				"movie_id", id, "err", h.scrub(err))
		}
		writePlaceholder(w)
		return
	}
	h.cache.put(id, img)
	writeJPEG(w, img)
}

// Outcomes of fetch other than success. errNoPoster (no TMDB id, unknown
// movie, no poster) is remembered in the negative cache and not logged;
// errLimited is neither; a transient error (the movie lookup itself
// failed, e.g. recs-model down) is logged but not remembered; any other
// error (a TMDB failure) is logged and remembered.
var (
	errNoPoster = errors.New("no poster")
	errLimited  = errors.New("rate limited")
)

type transient struct{ err error }

func (t transient) Error() string { return t.err.Error() }
func (t transient) Unwrap() error { return t.err }

// fetch runs steps 2-5 above.
func (h *handler) fetch(ctx context.Context, id int64) ([]byte, error) {
	if allowed, _ := h.lookup.Allow(); !allowed {
		return nil, errLimited
	}
	res, err := h.movies.Get(ctx, []int64{id})
	m, ok := res.Movies[id]
	if !ok {
		if err != nil {
			return nil, transient{fmt.Errorf("movie lookup: %w", err)}
		}
		return nil, errNoPoster
	}
	if m.TMDBID == nil || *m.TMDBID <= 0 {
		return nil, errNoPoster
	}
	if allowed, _ := h.limiter.Allow(); !allowed {
		return nil, errLimited
	}
	ctx, cancel := context.WithTimeout(ctx, h.timeout)
	defer cancel()

	// Step 4: the poster path.
	api := h.apiBase.JoinPath("3", "movie", strconv.FormatInt(*m.TMDBID, 10))
	q := url.Values{}
	q.Set("api_key", h.key.Reveal())
	api.RawQuery = q.Encode()
	body, err := h.get(ctx, api, maxAPIBytes, "application/json")
	if err != nil {
		return nil, fmt.Errorf("tmdb api: %w", err)
	}
	var details struct {
		PosterPath *string `json:"poster_path"`
	}
	if err := json.Unmarshal(body, &details); err != nil {
		return nil, errors.New("tmdb api: response is not the expected JSON")
	}
	if details.PosterPath == nil {
		return nil, errNoPoster
	}
	if !posterPath.MatchString(*details.PosterPath) {
		return nil, errors.New("tmdb api: unexpected poster_path")
	}

	// Step 5: the image. No key goes to the image host.
	imgURL := h.imageBase.JoinPath("t", "p", "w342", strings.TrimPrefix(*details.PosterPath, "/"))
	img, err := h.get(ctx, imgURL, MaxImageBytes, "")
	if err != nil {
		return nil, fmt.Errorf("tmdb image: %w", err)
	}
	if !isJPEG(img) {
		return nil, errors.New("tmdb image: not a JPEG")
	}
	return img, nil
}

// get fetches u (which must be on one of the two TMDB hosts) and returns
// its body, refusing non-200 responses and bodies over limit.
func (h *handler) get(ctx context.Context, u *url.URL, limit int64, accept string) ([]byte, error) {
	if !h.allowedHost(u) {
		return nil, errors.New("host not allowed")
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
	if err != nil {
		return nil, errors.New("building request failed")
	}
	if accept != "" {
		req.Header.Set("Accept", accept)
	}
	resp, err := h.client.Do(req)
	if err != nil {
		// A *url.Error repeats the full URL (with the key); keep only the cause.
		var ue *url.Error
		if errors.As(err, &ue) {
			err = ue.Err
		}
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4<<10))
		return nil, fmt.Errorf("status %d", resp.StatusCode)
	}
	if resp.ContentLength > limit {
		return nil, fmt.Errorf("response of %d bytes is over the %d byte limit", resp.ContentLength, limit)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, limit+1))
	if err != nil {
		var ue *url.Error
		if errors.As(err, &ue) {
			err = ue.Err
		}
		return nil, err
	}
	if int64(len(body)) > limit {
		return nil, fmt.Errorf("response is over the %d byte limit", limit)
	}
	return body, nil
}

// allowedHost reports whether u is on the TMDB API or image host, with the
// configured scheme.
func (h *handler) allowedHost(u *url.URL) bool {
	for _, b := range []*url.URL{h.apiBase, h.imageBase} {
		if u.Scheme == b.Scheme && strings.EqualFold(u.Host, b.Host) {
			return true
		}
	}
	return false
}

// checkRedirect allows a redirect only to the same scheme and host as the
// original request, at most maxRedirects times.
func (h *handler) checkRedirect(req *http.Request, via []*http.Request) error {
	if len(via) >= maxRedirects {
		return errors.New("too many redirects")
	}
	first := via[0].URL
	if req.URL.Scheme != first.Scheme || !strings.EqualFold(req.URL.Host, first.Host) {
		return errors.New("redirect to another host refused")
	}
	return nil
}

// scrub turns err into log text with the key removed, in case some error
// ever carries it.
func (h *handler) scrub(err error) string {
	s := err.Error()
	if k := h.key.Reveal(); k != "" {
		s = strings.ReplaceAll(s, k, "[redacted]")
		s = strings.ReplaceAll(s, url.QueryEscape(k), "[redacted]")
	}
	return s
}

// isJPEG checks the JPEG start-of-image marker.
func isJPEG(b []byte) bool {
	return len(b) > 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF
}

func writeJPEG(w http.ResponseWriter, img []byte) {
	w.Header().Set("Content-Type", "image/jpeg")
	w.Header().Set("Content-Length", strconv.Itoa(len(img)))
	w.Header().Set("Cache-Control", "public, max-age=604800")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(img)
}

// writePlaceholder serves the SVG. It is cached briefly so a poster that
// failed is retried later. The CSP keeps the SVG inert if opened directly.
func writePlaceholder(w http.ResponseWriter) {
	w.Header().Set("Content-Type", "image/svg+xml")
	w.Header().Set("Cache-Control", "public, max-age=300")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; sandbox")
	w.WriteHeader(http.StatusOK)
	_, _ = io.WriteString(w, placeholderSVG)
}
