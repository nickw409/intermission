package modelclient

import (
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/nickw409/intermission/api/internal/contractsdir"
)

func enum(t *testing.T, src []byte, re string) []string {
	t.Helper()
	m := regexp.MustCompile(re).FindSubmatch(src)
	if m == nil {
		t.Fatalf("pattern %q not found in model.yaml", re)
	}
	var out []string
	for _, f := range strings.Split(string(m[1]), ",") {
		out = append(out, strings.TrimSpace(f))
	}
	return out
}

// bounds returns the two submatches of re in src, or nil.
func bounds(src []byte, re string) []string {
	m := regexp.MustCompile(re).FindSubmatch(src)
	if len(m) != 3 {
		return nil
	}
	return []string{string(m[1]), string(m[2])}
}

// The Go enums must match contracts/model.yaml exactly.
func TestEnumsMatchContract(t *testing.T) {
	dir, err := contractsdir.Find()
	if err != nil {
		t.Fatal(err)
	}
	src, err := os.ReadFile(filepath.Join(dir, "model.yaml"))
	if err != nil {
		t.Fatal(err)
	}

	var models []string
	for _, m := range AllModels {
		models = append(models, string(m))
	}
	if got, want := strings.Join(models, ","), strings.Join(enum(t, src, `ModelName: \{ type: string, enum: \[([^\]]*)\]`), ","); got != want {
		t.Errorf("ModelName: go=%s contract=%s", got, want)
	}

	var genres []string
	for _, g := range AllGenres {
		genres = append(genres, string(g))
	}
	genreEnum := `Genre:\s*\n\s*type: string\n\s*description: [^\n]*\n\s*enum: \[([^\]]*)\]`
	if got, want := strings.Join(genres, ","), strings.Join(enum(t, src, genreEnum), ","); got != want {
		t.Errorf("Genre (model.yaml): go=%s contract=%s", got, want)
	}
	// The public API accepts exactly the same names (public.yaml Genre).
	pub, err := os.ReadFile(filepath.Join(dir, "public.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(genres, ","), strings.Join(enum(t, pub, genreEnum), ","); got != want {
		t.Errorf("Genre (public.yaml): go=%s contract=%s", got, want)
	}
	if len(AllGenres) != MaxGenres {
		t.Errorf("MaxGenres %d, %d genres", MaxGenres, len(AllGenres))
	}
	if got, want := MaxGenres, atoiT(t, enum(t, src, `Genres:\s*\n\s*type: array\n\s*maxItems: (\d+)`)[0]); got != want {
		t.Errorf("MaxGenres %d, model.yaml Genres maxItems %d", got, want)
	}
	// The release-year bounds (model.yaml ReleaseYear; public.yaml YearFrom
	// and YearTo).
	yr := bounds(src, `ReleaseYear:\s*\n\s*type: integer\n\s*minimum: (\d+)\n\s*maximum: (\d+)`)
	if len(yr) != 2 || atoiT(t, yr[0]) != MinYear || atoiT(t, yr[1]) != MaxYear {
		t.Errorf("MinYear..MaxYear %d..%d, model.yaml ReleaseYear %v", MinYear, MaxYear, yr)
	}
	for _, p := range []string{"YearFrom", "YearTo"} {
		got := bounds(pub, p+`:\s*\n(?:\s+[A-Za-z]+: [^\n]*\n)*?\s*schema: \{ type: integer, minimum: (\d+), maximum: (\d+)`)
		if len(got) != 2 || atoiT(t, got[0]) != MinYear || atoiT(t, got[1]) != MaxYear {
			t.Errorf("MinYear..MaxYear %d..%d, public.yaml %s %v", MinYear, MaxYear, p, got)
		}
	}

	codes := []string{ErrorCodeBadRequest, ErrorCodeInvalidJSON, ErrorCodeBodyTooLarge, ErrorCodeNotFound,
		ErrorCodeUnknownModel, ErrorCodeModelUnavailable, ErrorCodeInternal}
	if got, want := strings.Join(codes, ","), strings.Join(enum(t, src, `code: \{ type: string, enum: \[([^\]]*)\]`), ","); got != want {
		t.Errorf("error codes: go=%s contract=%s", got, want)
	}
}

func atoiT(t *testing.T, s string) int {
	t.Helper()
	n, err := strconv.Atoi(s)
	if err != nil {
		t.Fatal(err)
	}
	return n
}
