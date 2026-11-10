package app

import (
	"bytes"
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"testing"
)

func env(m map[string]string) func(string) string {
	return func(k string) string { return m[k] }
}

func TestDefaults(t *testing.T) {
	c, err := LoadConfig(env(nil))
	if err != nil {
		t.Fatal(err)
	}
	if c.Addr != DefaultAddr || c.ModelSocket != "/run/recs/model.sock" || c.SignupMode != SignupInvite ||
		c.LogLevel != slog.LevelInfo || c.PosterCacheMaxByte != DefaultPosterCacheMaxByte || c.SiteOrigin != DefaultSiteOrigin {
		t.Fatalf("defaults = %+v", c)
	}
}

func TestOverrides(t *testing.T) {
	c, err := LoadConfig(env(map[string]string{
		"RECS_ADDR":                   "0.0.0.0:9000",
		"RECS_MODEL_ADDR":             "127.0.0.1:8090",
		"RECS_SITE_ORIGIN":            "HTTPS://Example.ORG/",
		"RECS_SIGNUP_MODE":            "open",
		"RECS_POSTER_CACHE_MAX_BYTES": "2097152",
		"RECS_LOG_LEVEL":              "debug",
		"TMDB_API_KEY":                "k-123",
	}))
	if err != nil {
		t.Fatal(err)
	}
	if c.Addr != "0.0.0.0:9000" || c.ModelAddr != "127.0.0.1:8090" || c.SiteOrigin != "https://example.org" ||
		c.SignupMode != SignupOpen || c.PosterCacheMaxByte != 2097152 || c.LogLevel != slog.LevelDebug ||
		c.TMDBAPIKey.Reveal() != "k-123" {
		t.Fatalf("config = %+v", c)
	}
}

func TestInvalidReportsEveryProblem(t *testing.T) {
	_, err := LoadConfig(env(map[string]string{
		"RECS_ADDR":                   "nope",
		"RECS_MODEL_ADDR":             "x",
		"RECS_MODEL_SOCKET":           "relative.sock",
		"RECS_SITE_ORIGIN":            "https://example.org/path",
		"RECS_SIGNUP_MODE":            "closed",
		"RECS_POSTER_CACHE_MAX_BYTES": "10",
		"RECS_LOG_LEVEL":              "loud",
	}))
	if err == nil {
		t.Fatal("want error")
	}
	for _, key := range []string{"RECS_ADDR", "RECS_MODEL_ADDR", "RECS_MODEL_SOCKET", "RECS_SITE_ORIGIN",
		"RECS_SIGNUP_MODE", "RECS_POSTER_CACHE_MAX_BYTES", "RECS_LOG_LEVEL"} {
		if !strings.Contains(err.Error(), key) {
			t.Errorf("error does not mention %s: %v", key, err)
		}
	}
}

func TestOriginValidation(t *testing.T) {
	for _, o := range []string{"ftp://x.org", "https://", "https://u:p@x.org", "https://x.org?a=1", "x.org"} {
		if _, err := LoadConfig(env(map[string]string{"RECS_SITE_ORIGIN": o})); err == nil {
			t.Errorf("origin %q accepted", o)
		}
	}
}

// The TMDB key must never show up in logs or formatted output.
func TestSecretNeverPrinted(t *testing.T) {
	const key = "super-secret-tmdb-key"
	c, err := LoadConfig(env(map[string]string{"TMDB_API_KEY": key}))
	if err != nil {
		t.Fatal(err)
	}
	var buf bytes.Buffer
	logger := slog.New(slog.NewJSONHandler(&buf, nil))
	logger.Info("config", "config", c, "key", c.TMDBAPIKey)
	js, _ := json.Marshal(c)
	outputs := []string{buf.String(), fmt.Sprintf("%v %+v %#v %s", c, c, c, c.TMDBAPIKey), string(js)}
	for _, out := range outputs {
		if strings.Contains(out, key) {
			t.Fatalf("secret leaked: %s", out)
		}
	}
	if !strings.Contains(buf.String(), `"tmdb_api_key_set":true`) {
		t.Fatalf("log = %s", buf.String())
	}
}
