package app

import (
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/url"
	"strconv"
	"strings"
)

// SignupMode controls who may create an account.
type SignupMode string

// Signup modes (spec: "Invite-only signup").
const (
	SignupInvite SignupMode = "invite"
	SignupOpen   SignupMode = "open"
)

// Secret is a string that never appears in logs or formatted output.
type Secret string

// String implements fmt.Stringer and always redacts.
func (s Secret) String() string {
	if s == "" {
		return ""
	}
	return "[redacted]"
}

// GoString implements fmt.GoStringer and always redacts.
func (s Secret) GoString() string { return strconv.Quote(s.String()) }

// LogValue implements slog.LogValuer and always redacts.
func (s Secret) LogValue() slog.Value { return slog.StringValue(s.String()) }

// MarshalText implements encoding.TextMarshaler (used by JSON) and always redacts.
func (s Secret) MarshalText() ([]byte, error) { return []byte(s.String()), nil }

// Reveal returns the secret value. Call it only where the value is sent to
// its destination (the TMDB request).
func (s Secret) Reveal() string { return string(s) }

// Config is recs-api's configuration, read from the environment.
type Config struct {
	Addr               string     // RECS_ADDR: listen address (host:port)
	DBPath             string     // RECS_DB_PATH: SQLite file
	ModelSocket        string     // RECS_MODEL_SOCKET: recs-model Unix socket
	ModelAddr          string     // RECS_MODEL_ADDR: TCP fallback host:port; overrides the socket when set
	SiteOrigin         string     // RECS_SITE_ORIGIN: scheme://host[:port], used for CSRF Origin checks
	SignupMode         SignupMode // RECS_SIGNUP_MODE: invite | open
	ReportsDir         string     // RECS_REPORTS_DIR: run reports written by recs-train
	ExportsDir         string     // RECS_EXPORTS_DIR: export-ratings output
	PosterCacheDir     string     // RECS_POSTER_CACHE_DIR
	PosterCacheMaxByte int64      // RECS_POSTER_CACHE_MAX_BYTES
	TMDBAPIKey         Secret     // TMDB_API_KEY: optional; posters fall back to placeholders without it
	LogLevel           slog.Level // RECS_LOG_LEVEL: debug | info | warn | error
}

// Defaults for unset variables.
const (
	DefaultAddr               = "127.0.0.1:8081"
	DefaultDBPath             = "data/app.db"
	DefaultModelSocket        = "/run/recs/model.sock"
	DefaultSiteOrigin         = "http://localhost:8080"
	DefaultReportsDir         = "data/reports"
	DefaultExportsDir         = "data/exports"
	DefaultPosterCacheDir     = "data/posters"
	DefaultPosterCacheMaxByte = 512 << 20 // 512 MiB
)

// LoadConfig reads the configuration using getenv (normally os.Getenv) and
// validates it. All problems are reported together.
func LoadConfig(getenv func(string) string) (Config, error) {
	get := func(key, def string) string {
		if v := strings.TrimSpace(getenv(key)); v != "" {
			return v
		}
		return def
	}
	var errs []error
	bad := func(key, format string, args ...any) {
		errs = append(errs, fmt.Errorf("%s: %s", key, fmt.Sprintf(format, args...)))
	}

	c := Config{
		Addr:           get("RECS_ADDR", DefaultAddr),
		DBPath:         get("RECS_DB_PATH", DefaultDBPath),
		ModelSocket:    get("RECS_MODEL_SOCKET", DefaultModelSocket),
		ModelAddr:      get("RECS_MODEL_ADDR", ""),
		SiteOrigin:     get("RECS_SITE_ORIGIN", DefaultSiteOrigin),
		SignupMode:     SignupMode(get("RECS_SIGNUP_MODE", string(SignupInvite))),
		ReportsDir:     get("RECS_REPORTS_DIR", DefaultReportsDir),
		ExportsDir:     get("RECS_EXPORTS_DIR", DefaultExportsDir),
		PosterCacheDir: get("RECS_POSTER_CACHE_DIR", DefaultPosterCacheDir),
		TMDBAPIKey:     Secret(strings.TrimSpace(getenv("TMDB_API_KEY"))),
	}

	if _, port, err := net.SplitHostPort(c.Addr); err != nil || port == "" {
		bad("RECS_ADDR", "%q is not host:port", c.Addr)
	}
	if c.ModelAddr != "" {
		if _, port, err := net.SplitHostPort(c.ModelAddr); err != nil || port == "" {
			bad("RECS_MODEL_ADDR", "%q is not host:port", c.ModelAddr)
		}
	}
	if !strings.HasPrefix(c.ModelSocket, "/") {
		bad("RECS_MODEL_SOCKET", "%q must be an absolute path", c.ModelSocket)
	}
	if origin, err := normalizeOrigin(c.SiteOrigin); err != nil {
		bad("RECS_SITE_ORIGIN", "%v", err)
	} else {
		c.SiteOrigin = origin
	}
	switch c.SignupMode {
	case SignupInvite, SignupOpen:
	default:
		bad("RECS_SIGNUP_MODE", "%q must be invite or open", c.SignupMode)
	}

	c.PosterCacheMaxByte = DefaultPosterCacheMaxByte
	if v := get("RECS_POSTER_CACHE_MAX_BYTES", ""); v != "" {
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil || n < 1<<20 {
			bad("RECS_POSTER_CACHE_MAX_BYTES", "%q must be an integer >= 1048576", v)
		} else {
			c.PosterCacheMaxByte = n
		}
	}

	if err := c.LogLevel.UnmarshalText([]byte(get("RECS_LOG_LEVEL", "info"))); err != nil {
		bad("RECS_LOG_LEVEL", "%q must be debug, info, warn or error", get("RECS_LOG_LEVEL", ""))
	}

	if len(errs) > 0 {
		return Config{}, fmt.Errorf("invalid configuration: %w", errors.Join(errs...))
	}
	return c, nil
}

// normalizeOrigin checks that s is a bare origin (scheme://host[:port]) and
// returns it in canonical form (lowercase scheme and host, no trailing slash).
func normalizeOrigin(s string) (string, error) {
	u, err := url.Parse(s)
	if err != nil {
		return "", fmt.Errorf("%q is not a URL", s)
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return "", fmt.Errorf("%q must use http or https", s)
	}
	if u.Host == "" || u.User != nil || (u.Path != "" && u.Path != "/") || u.RawQuery != "" || u.Fragment != "" {
		return "", fmt.Errorf("%q must be an origin like https://example.org", s)
	}
	return strings.ToLower(u.Scheme) + "://" + strings.ToLower(u.Host), nil
}

// LogValue implements slog.LogValuer so the config can be logged at startup
// without leaking the TMDB key.
func (c Config) LogValue() slog.Value {
	return slog.GroupValue(
		slog.String("addr", c.Addr),
		slog.String("db_path", c.DBPath),
		slog.String("model_socket", c.ModelSocket),
		slog.String("model_addr", c.ModelAddr),
		slog.String("site_origin", c.SiteOrigin),
		slog.String("signup_mode", string(c.SignupMode)),
		slog.String("reports_dir", c.ReportsDir),
		slog.String("exports_dir", c.ExportsDir),
		slog.String("poster_cache_dir", c.PosterCacheDir),
		slog.Int64("poster_cache_max_bytes", c.PosterCacheMaxByte),
		slog.Bool("tmdb_api_key_set", c.TMDBAPIKey != ""),
		slog.String("log_level", c.LogLevel.String()),
	)
}
