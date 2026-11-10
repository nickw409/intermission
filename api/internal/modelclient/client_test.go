package modelclient

import (
	"os"
	"path/filepath"
	"regexp"
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

	codes := []string{ErrorCodeBadRequest, ErrorCodeInvalidJSON, ErrorCodeBodyTooLarge, ErrorCodeNotFound,
		ErrorCodeUnknownModel, ErrorCodeModelUnavailable, ErrorCodeInternal}
	if got, want := strings.Join(codes, ","), strings.Join(enum(t, src, `code: \{ type: string, enum: \[([^\]]*)\]`), ","); got != want {
		t.Errorf("error codes: go=%s contract=%s", got, want)
	}
}
