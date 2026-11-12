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
//  5. The cleaned MovieDetails is stored as JSON and served.
//
// Any failure serves the empty MovieDetails (all null, no directors, no
// cast) with 200 and a short Cache-Control, so the movie page just shows
// less. Nothing here ever answers 5xx.

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

	detailsMaxAge = "public, max-age=86400"
	emptyMaxAge   = "public, max-age=300"
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

// detailsFiles are cleaned MovieDetails as JSON.
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
		// TMDB. A file for another movie or one that doesn't decode is a
		// miss and is refetched (and overwritten).
		if d, ok := decodeCached(b, id); ok {
			writeDetails(w, d, detailsMaxAge)
			return
		}
	}
	if h.key == "" || h.detailsNegative.has(id) {
		writeDetails(w, emptyDetails(id), emptyMaxAge)
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
		writeDetails(w, emptyDetails(id), emptyMaxAge)
		return
	}
	d, ok := decodeCached(call.img, id)
	if !ok {
		writeDetails(w, emptyDetails(id), emptyMaxAge)
		return
	}
	writeDetails(w, d, detailsMaxAge)
}

// fetchDetailsAndRecord fetches id's details and records the outcome in the
// details disk cache or negative cache. It returns the cleaned JSON.
func (h *handler) fetchDetailsAndRecord(ctx context.Context, id int64) ([]byte, error) {
	d, err := h.fetchDetails(ctx, id)
	if err != nil {
		h.noteFailure(ctx, "details", id, err, h.detailsNegative)
		return nil, err
	}
	b, err := json.Marshal(d)
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
	Order     int     `json:"order"` // billing order, 0 first
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
		if _, err := time.Parse(time.DateOnly, *raw.ReleaseDate); err == nil {
			rd := *raw.ReleaseDate
			d.ReleaseDate = &rd
		}
	}

	cast := slices.Clone(raw.Credits.Cast)
	slices.SortStableFunc(cast, func(a, b tmdbCast) int { return cmp.Compare(a.Order, b.Order) })
	for _, c := range cast {
		if len(d.Cast) == maxCast {
			break
		}
		name := cleanText(c.Name, maxNameRunes, false)
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
		if name := cleanText(c.Name, maxNameRunes, false); name != nil && !slices.Contains(d.Directors, *name) {
			d.Directors = append(d.Directors, *name)
		}
	}
	return d
}

// cleanText returns s made safe to show as plain text, or nil if nothing is
// left. Invalid UTF-8, control characters and bidi controls are dropped;
// tabs become spaces; newlines are kept (as \n) only if multiline. Text
// longer than max runes is cut to max runes, the last being an ellipsis.
func cleanText(s *string, max int, multiline bool) *string {
	if s == nil {
		return nil
	}
	var b strings.Builder
	for _, r := range strings.ToValidUTF8(*s, "") {
		switch {
		case r == '\n' || r == '\t':
			if multiline && r == '\n' {
				b.WriteRune('\n')
			} else {
				b.WriteRune(' ')
			}
		case unicode.IsControl(r) || unicode.Is(unicode.Bidi_Control, r) || r == utf8.RuneError:
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

// decodeCached decodes cached details for id and cleans them again.
func decodeCached(b []byte, id int64) (MovieDetails, bool) {
	var d MovieDetails
	if err := json.Unmarshal(b, &d); err != nil || d.MovieID != id {
		return MovieDetails{}, false
	}
	raw := tmdbDetails{Overview: d.Overview, Tagline: d.Tagline, Runtime: d.RuntimeMinutes, ReleaseDate: d.ReleaseDate}
	for i, c := range d.Cast {
		name := c.Name
		raw.Credits.Cast = append(raw.Credits.Cast, tmdbCast{Name: &name, Character: c.Character, Order: i})
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
