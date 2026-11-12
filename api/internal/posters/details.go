package posters

import (
	"cmp"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"slices"
	"strconv"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"

	"github.com/nickw409/intermission/api/internal/apperr"
	"github.com/nickw409/intermission/api/internal/moviecache"
)

// How movie details are served (GET /api/movies/{movieId}/details):
//
// The plot summary, tagline, runtime, release date, directors and top-billed
// cast come from TMDB's movie endpoint with append_to_response=credits, so
// one TMDB request covers both. The flow is the poster's (register.go) with
// JSON instead of a JPEG:
//
//  1. The movie ID is parsed and the details disk cache checked (a sibling
//     "details" directory of the poster cache, its own size cap). The
//     negative cache and a missing TMDB key stop here too.
//  2. The same global lookup limiter as posters, then the TMDB ID from the
//     movie cache.
//  3. The same global upstream limiter as posters: details and posters
//     share one TMDB budget.
//  4. The TMDB response is size-capped, decoded into only the fields we
//     use, and cleaned (see clean): TMDB text is untrusted.
//  5. The cleaned MovieDetails is stored as JSON with its fetch time and
//     served. A cached entry older than detailsTTL is refetched, so details
//     TMDB fills in later (new releases often lack cast) do show up.
//
// Any failure serves the empty MovieDetails (all null, no directors, no
// cast) with 200, so the movie page just shows less. Nothing here ever
// answers 5xx. A TMDB answer with nothing usable is treated like "no
// details" (remembered for negativeTTL, never written to disk). Failures
// are remembered and cached by browsers exactly as for posters (busyTTL
// for a TMDB 429 or 5xx, no-store when our own budget ran out; see
// failureCacheControl).

// Limits on details.
const (
	// maxDetailsBytes caps one TMDB details+credits response. Big
	// productions list hundreds of crew; ~300 KB is the largest seen.
	maxDetailsBytes = 2 << 20
	// maxDetailsFile caps one cached details file (the cleaned JSON is a
	// few KB at most).
	maxDetailsFile = 64 << 10
	// detailsCacheBytes caps the details disk cache: about 30,000 movies
	// at ~2 KB each.
	detailsCacheBytes = 64 << 20

	maxOverviewRunes = 2000
	maxTaglineRunes  = 300
	maxNameRunes     = 200
	maxCast          = 10
	maxDirectors     = 3
	maxRuntime       = 1500

	// maxCombiningRun caps a run of combining marks on one base character
	// (real scripts need a few; "Zalgo" text stacks dozens).
	maxCombiningRun = 4
	// minReleaseYear rejects placeholder dates such as 0001-01-01.
	minReleaseYear = 1800

	// detailsTTL is how long a cached entry is served before a refetch.
	detailsTTL = 30 * 24 * time.Hour

	detailsMaxAge = "public, max-age=86400"
)

// MovieDetails is the MovieDetails schema (contracts/public.yaml).
type MovieDetails struct {
	MovieID        int64        `json:"movieId"`
	Overview       *string      `json:"overview"`
	Tagline        *string      `json:"tagline"`
	RuntimeMinutes *int         `json:"runtime_minutes"`
	ReleaseDate    *string      `json:"release_date"`
	Directors      []string     `json:"directors"`
	Cast           []CastMember `json:"cast"`
}

// CastMember is one entry of MovieDetails.cast.
type CastMember struct {
	Name      string  `json:"name"`
	Character *string `json:"character"`
}

func emptyDetails(id int64) MovieDetails {
	return MovieDetails{MovieID: id, Directors: []string{}, Cast: []CastMember{}}
}

func (d MovieDetails) isEmpty() bool {
	return d.Overview == nil && d.Tagline == nil && d.RuntimeMinutes == nil && d.ReleaseDate == nil &&
		len(d.Directors) == 0 && len(d.Cast) == 0
}

// cachedDetails is what a details cache file holds.
type cachedDetails struct {
	FetchedAt int64        `json:"fetched_at"` // Unix seconds
	Details   MovieDetails `json:"details"`
}

// detailsFiles are cachedDetails as JSON.
var detailsFiles = fileKind{ext: ".json", maxFile: maxDetailsFile, valid: json.Valid}

var errNoDetails = errors.New("no details")

func (h *handler) serveDetails(w http.ResponseWriter, r *http.Request) {
	id, err := moviecache.ParseMovieID(r.PathValue("movieId"))
	if err != nil {
		apperr.Write(w, err)
		return
	}
	if b, ok := h.details.get(id); ok {
		// Decoded and cleaned again: a file on disk is trusted no more than
		// TMDB. A file for another movie, one that doesn't decode, or one
		// older than detailsTTL is a miss and is refetched (and overwritten).
		if d, ok := decodeCached(b, id, h.now()); ok {
			writeDetails(w, d, detailsMaxAge)
			return
		}
	}
	if h.key == "" {
		writeDetails(w, emptyDetails(id), maxAge(maxAgeFailure))
		return
	}
	if left, ok := h.detailsNegative.remaining(id); ok {
		writeDetails(w, emptyDetails(id), rememberedCacheControl(left))
		return
	}
	call := h.detailFlights.do(id, func() ([]byte, error) {
		return h.fetchDetailsAndRecord(context.WithoutCancel(r.Context()), id)
	})
	select {
	case <-call.done:
	case <-r.Context().Done():
		w.WriteHeader(499) // as for posters: client closed request
		return
	}
	if call.err != nil {
		writeDetails(w, emptyDetails(id), failureCacheControl(call.err))
		return
	}
	d, ok := decodeCached(call.img, id, h.now())
	if !ok {
		writeDetails(w, emptyDetails(id), maxAge(maxAgeTransient))
		return
	}
	writeDetails(w, d, detailsMaxAge)
}

// fetchDetailsAndRecord fetches id's details and records the outcome in the
// details disk cache or negative cache. It returns the cleaned JSON.
func (h *handler) fetchDetailsAndRecord(ctx context.Context, id int64) ([]byte, error) {
	d, err := h.fetchDetails(ctx, id)
	if err == nil && d.isEmpty() {
		err = errNoDetails
	}
	if err != nil {
		h.noteFailure(ctx, "details", id, err, h.detailsNegative)
		return nil, err
	}
	b, err := json.Marshal(cachedDetails{FetchedAt: h.now().Unix(), Details: d})
	if err != nil {
		return nil, err
	}
	h.details.put(id, b)
	return b, nil
}

// fetchDetails runs steps 2-4 above.
func (h *handler) fetchDetails(ctx context.Context, id int64) (MovieDetails, error) {
	if allowed, _ := h.lookup.Allow(); !allowed {
		return MovieDetails{}, errLimited
	}
	res, err := h.movies.Get(ctx, []int64{id})
	m, ok := res.Movies[id]
	if !ok {
		if err != nil {
			return MovieDetails{}, transient{fmt.Errorf("movie lookup: %w", err)}
		}
		return MovieDetails{}, errNoDetails
	}
	if m.TMDBID == nil || *m.TMDBID <= 0 {
		return MovieDetails{}, errNoDetails
	}
	if allowed, _ := h.limiter.Allow(); !allowed {
		return MovieDetails{}, errLimited
	}
	ctx, cancel := context.WithTimeout(ctx, h.timeout)
	defer cancel()

	api := h.apiBase.JoinPath("3", "movie", strconv.FormatInt(*m.TMDBID, 10))
	q := url.Values{}
	q.Set("api_key", h.key.Reveal())
	q.Set("append_to_response", "credits")
	api.RawQuery = q.Encode()
	body, err := h.get(ctx, api, maxDetailsBytes, "application/json")
	if err != nil {
		return MovieDetails{}, fmt.Errorf("tmdb details: %w", err)
	}
	var raw tmdbDetails
	if err := json.Unmarshal(body, &raw); err != nil {
		return MovieDetails{}, errors.New("tmdb details: response is not the expected JSON")
	}
	return clean(id, raw), nil
}

// tmdbDetails is the part of TMDB's /3/movie/{id}?append_to_response=credits
// response that we use. Everything else is ignored.
type tmdbDetails struct {
	Overview    *string `json:"overview"`
	Tagline     *string `json:"tagline"`
	Runtime     *int    `json:"runtime"`
	ReleaseDate *string `json:"release_date"`
	Credits     struct {
		Cast []tmdbCast `json:"cast"`
		Crew []tmdbCrew `json:"crew"`
	} `json:"credits"`
}

type tmdbCast struct {
	Name      *string `json:"name"`
	Character *string `json:"character"`
	Order     *int    `json:"order"` // billing order, 0 first
}

type tmdbCrew struct {
	Name *string `json:"name"`
	Job  *string `json:"job"`
}

// clean turns TMDB's response into a MovieDetails that meets the contract:
// text is valid UTF-8 without control or bidi-override characters, trimmed
// and capped in length; empty values become null; the runtime and date must
// be plausible; the cast is the first maxCast in billing order and the
// directors the first maxDirectors distinct names.
func clean(id int64, raw tmdbDetails) MovieDetails {
	d := emptyDetails(id)
	d.Overview = cleanText(raw.Overview, maxOverviewRunes, true)
	d.Tagline = cleanText(raw.Tagline, maxTaglineRunes, false)
	if raw.Runtime != nil && *raw.Runtime >= 1 && *raw.Runtime <= maxRuntime {
		rt := *raw.Runtime
		d.RuntimeMinutes = &rt
	}
	if raw.ReleaseDate != nil && len(*raw.ReleaseDate) == len(time.DateOnly) {
		if t, err := time.Parse(time.DateOnly, *raw.ReleaseDate); err == nil && t.Year() >= minReleaseYear {
			rd := *raw.ReleaseDate
			d.ReleaseDate = &rd
		}
	}

	cast := slices.Clone(raw.Credits.Cast)
	// Billing order; an entry without one goes after every entry with one.
	slices.SortStableFunc(cast, func(a, b tmdbCast) int {
		switch {
		case a.Order == nil && b.Order == nil:
			return 0
		case a.Order == nil:
			return 1
		case b.Order == nil:
			return -1
		}
		return cmp.Compare(*a.Order, *b.Order)
	})
	for _, c := range cast {
		if len(d.Cast) == maxCast {
			break
		}
		name := cleanName(c.Name)
		if name == nil {
			continue
		}
		d.Cast = append(d.Cast, CastMember{Name: *name, Character: cleanText(c.Character, maxNameRunes, false)})
	}

	for _, c := range raw.Credits.Crew {
		if len(d.Directors) == maxDirectors {
			break
		}
		if c.Job == nil || *c.Job != "Director" {
			continue
		}
		if name := cleanName(c.Name); name != nil && !slices.Contains(d.Directors, *name) {
			d.Directors = append(d.Directors, *name)
		}
	}
	return d
}

// cleanText returns s made safe to show as plain text, or nil if nothing is
// left. Invalid UTF-8, control characters and invisible format characters
// (bidi controls, zero-width spaces, BOMs, tags; Unicode category Cf) are
// dropped, except the zero-width joiner and non-joiner that some scripts
// and emoji need. Runs of combining marks are capped at maxCombiningRun.
// Tabs become spaces; newlines are kept (as \n) only if multiline. Text
// longer than max runes is cut to max runes, the last being an ellipsis.
func cleanText(s *string, max int, multiline bool) *string {
	if s == nil {
		return nil
	}
	var b strings.Builder
	marks := 0 // combining marks since the last base character
	for _, r := range strings.ToValidUTF8(*s, "") {
		if unicode.Is(unicode.Mn, r) || unicode.Is(unicode.Me, r) {
			if marks++; marks > maxCombiningRun {
				continue
			}
		} else {
			marks = 0
		}
		switch {
		case r == '\n' || r == '\t':
			if multiline && r == '\n' {
				b.WriteRune('\n')
			} else {
				b.WriteRune(' ')
			}
		case r == zwnj || r == zwj:
			b.WriteRune(r)
		case unicode.IsControl(r) || unicode.Is(unicode.Cf, r) || r == utf8.RuneError:
			// dropped (\r included, so CRLF becomes \n)
		default:
			b.WriteRune(r)
		}
	}
	out := strings.TrimSpace(b.String())
	if out == "" {
		return nil
	}
	if utf8.RuneCountInString(out) > max {
		runes := []rune(out)[:max-1]
		out = strings.TrimRightFunc(string(runes), unicode.IsSpace) + "…"
	}
	return &out
}

const (
	zwnj = '\u200C' // zero-width non-joiner
	zwj  = '\u200D' // zero-width joiner
)

// cleanName is cleanText for a person's name, which must also contain at
// least one letter or digit (so it can't render as blank).
func cleanName(s *string) *string {
	name := cleanText(s, maxNameRunes, false)
	if name == nil || !strings.ContainsFunc(*name, func(r rune) bool { return unicode.IsLetter(r) || unicode.IsNumber(r) }) {
		return nil
	}
	return name
}

// decodeCached decodes a details cache file for id and cleans it again. It
// fails for another movie's file, a file that doesn't decode, and one
// fetched more than detailsTTL before now (or in the future).
func decodeCached(b []byte, id int64, now time.Time) (MovieDetails, bool) {
	var c cachedDetails
	if err := json.Unmarshal(b, &c); err != nil || c.Details.MovieID != id {
		return MovieDetails{}, false
	}
	fetched := time.Unix(c.FetchedAt, 0)
	if c.FetchedAt <= 0 || now.Sub(fetched) > detailsTTL || fetched.After(now.Add(time.Hour)) {
		return MovieDetails{}, false
	}
	d := c.Details
	raw := tmdbDetails{Overview: d.Overview, Tagline: d.Tagline, Runtime: d.RuntimeMinutes, ReleaseDate: d.ReleaseDate}
	for i, c := range d.Cast {
		name := c.Name
		order := i
		raw.Credits.Cast = append(raw.Credits.Cast, tmdbCast{Name: &name, Character: c.Character, Order: &order})
	}
	director := "Director"
	for _, n := range d.Directors {
		name := n
		raw.Credits.Crew = append(raw.Credits.Crew, tmdbCrew{Name: &name, Job: &director})
	}
	return clean(id, raw), true
}

func writeDetails(w http.ResponseWriter, d MovieDetails, cacheControl string) {
	w.Header().Set("Cache-Control", cacheControl)
	apperr.WriteJSON(w, http.StatusOK, d)
}
