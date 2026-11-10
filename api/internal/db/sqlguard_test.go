package db

import (
	"bufio"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// A cheap guard against string-built SQL anywhere in recs-api: no non-test Go
// line may both mention an SQL keyword inside a string literal and build a
// string with fmt.Sprintf / string concatenation. A line that genuinely needs
// it (an integer the program itself computed, never input) carries a
// "sqlguard:allow <reason>" comment and is reviewed by hand.
var (
	sqlKeyword  = regexp.MustCompile(`"[^"]*\b(SELECT|INSERT|UPDATE|DELETE FROM|REPLACE INTO|WHERE|PRAGMA|VACUUM|CREATE|DROP|ALTER)\b[^"]*"`)
	stringBuild = regexp.MustCompile(`fmt\.(Sprintf|Fprintf|Appendf)\(|"\s*\+|\+\s*"|strings\.Join\(|\.WriteString\(`)
)

func TestNoStringBuiltSQL(t *testing.T) {
	root := filepath.Join("..", "..") // the api module root
	var offenders []string
	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() || !strings.HasSuffix(path, ".go") || strings.HasSuffix(path, "_test.go") {
			return nil
		}
		f, err := os.Open(path)
		if err != nil {
			return err
		}
		defer f.Close()
		sc := bufio.NewScanner(f)
		for n := 1; sc.Scan(); n++ {
			line := sc.Text()
			trimmed := strings.TrimSpace(line)
			if strings.HasPrefix(trimmed, "//") || strings.Contains(line, "sqlguard:allow") {
				continue
			}
			if sqlKeyword.MatchString(line) && stringBuild.MatchString(line) {
				offenders = append(offenders, path+":"+strconv.Itoa(n)+": "+trimmed)
			}
		}
		return sc.Err()
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, o := range offenders {
		t.Errorf("possible string-built SQL: %s", o)
	}
}

// The guard itself must catch the obvious forms.
func TestSQLGuardCatchesExamples(t *testing.T) {
	for _, bad := range []string{
		`q := fmt.Sprintf("SELECT * FROM users WHERE name = '%s'", name)`,
		`q := "SELECT * FROM users WHERE name = '" + name + "'"`,
		`db.Exec("DELETE FROM sessions WHERE id = " + id)`,
	} {
		if !(sqlKeyword.MatchString(bad) && stringBuild.MatchString(bad)) {
			t.Errorf("guard missed: %s", bad)
		}
	}
	for _, good := range []string{
		`db.Exec("DELETE FROM sessions WHERE token_hash = ?", h)`,
		`mux.HandleFunc("DELETE /api/me", h)`,
	} {
		if sqlKeyword.MatchString(good) && stringBuild.MatchString(good) {
			t.Errorf("guard false positive: %s", good)
		}
	}
}
