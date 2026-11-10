package export

import (
	"bufio"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/db"
)

// The file format is contracts/export.md (format_version 1).

// Header is the exact first line of every export.
const Header = "site_user_id,movie_id,rating,timestamp"

// Keep is how many exports are retained.
const Keep = 7

// MaxAge: an export this old (by the timestamp in its name) is deleted
// whatever Keep says, so a deleted account's ratings leave the exports
// within a week even if the nightly job paused. The export LATEST names
// and the one just written are never deleted (see prune).
const MaxAge = 7 * 24 * time.Hour

// exportLayout is the timestamp format in export file names (UTC).
const exportLayout = "2006-01-02T15-04-05Z"

// LatestName is the pointer file holding the newest export's file name.
const LatestName = "LATEST"

// exportName matches the files this package writes (and may delete).
var exportName = regexp.MustCompile(`^ratings-\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z\.csv$`)

// tempName matches this package's temp files (os.CreateTemp pattern below).
var tempName = regexp.MustCompile(`^\.export-\d+\.tmp$`)

// staleTemp is the age after which a leftover temp file (from a crashed
// export) is removed. Younger ones may belong to an export still running.
const staleTemp = time.Hour

// Result describes one export.
type Result struct {
	Path  string
	Rows  int
	Users int
}

// Run executes `recs-api export-ratings [--dir DIR]`. args are the
// arguments after the subcommand name. DIR defaults to RECS_EXPORTS_DIR.
func Run(ctx context.Context, d *app.Deps, args []string) error {
	fset := flag.NewFlagSet("export-ratings", flag.ContinueOnError)
	fset.SetOutput(io.Discard)
	dir := fset.String("dir", d.Config.ExportsDir, "output directory")
	if err := fset.Parse(args); err != nil {
		return fmt.Errorf("export-ratings: %w (usage: export-ratings [--dir DIR])", err)
	}
	if fset.NArg() != 0 {
		return fmt.Errorf("export-ratings: unexpected argument %q (usage: export-ratings [--dir DIR])", fset.Arg(0))
	}
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	now := time.Now()
	// Nightly housekeeping first, whatever happens to the export (security
	// audit SA5-3): drop pre-migration backups past retention and move the
	// WAL into app.db, so a deleted account's data never waits for the next
	// startup.
	d.DB.PruneBackups(logger)
	if err := d.DB.CheckpointTruncateRetry(ctx); err != nil {
		logger.Warn("nightly WAL checkpoint did not complete", "err", err)
	}
	res, err := Export(ctx, d.DB, *dir, now)
	if err != nil {
		logger.Error("ratings export failed; pruning old exports anyway", "err", err)
		if perr := prune(*dir, now, ""); perr != nil && !errors.Is(perr, fs.ErrNotExist) {
			logger.Warn("pruning old exports failed", "err", perr)
		}
		return err
	}
	logger.Info("ratings exported", "file", filepath.Base(res.Path), "rows", res.Rows, "users", res.Users)
	return nil
}

// Export writes every rating of every existing account to
// dir/ratings-<UTC time>.csv (temp file, fsync, hard link into place), points
// dir/LATEST at it (temp file, rename), and deletes all but the Keep newest
// exports and any stale temp files. Only internal user IDs are written, never
// usernames. An existing file of the same name is never overwritten (the
// link fails instead). dir is created 0750 if missing and refused if it is
// writable by group or others (anyone who could write there could swap the
// files recs-train reads).
func Export(ctx context.Context, database *db.DB, dir string, now time.Time) (Result, error) {
	if err := os.MkdirAll(dir, 0o750); err != nil {
		return Result{}, fmt.Errorf("export: create %s: %w", dir, err)
	}
	if err := checkDir(dir); err != nil {
		return Result{}, err
	}
	name := "ratings-" + now.UTC().Format(exportLayout) + ".csv"
	final := filepath.Join(dir, name)

	res := Result{Path: final}
	err := publishNew(dir, final, func(w *bufio.Writer) error {
		var err error
		res.Rows, res.Users, err = writeRows(ctx, database, w)
		return err
	})
	if err != nil {
		return Result{}, err
	}
	if err := replaceAtomically(dir, filepath.Join(dir, LatestName), func(w *bufio.Writer) error {
		_, err := w.WriteString(name)
		return err
	}); err != nil {
		return Result{}, err
	}
	if err := prune(dir, now, name); err != nil {
		return res, err
	}
	return res, nil
}

// writeRows writes the header and one line per rating, sorted by
// (site_user_id, movie_id). The join drops ratings of deleted accounts even
// if a cascade were ever missed.
func writeRows(ctx context.Context, database *db.DB, w *bufio.Writer) (rows, users int, err error) {
	if _, err := w.WriteString(Header + "\n"); err != nil {
		return 0, 0, err
	}
	rs, err := database.Reader.QueryContext(ctx,
		`SELECT r.user_id, r.movie_id, r.rating, r.updated_at
		   FROM ratings AS r JOIN users AS u ON u.id = r.user_id
		  ORDER BY r.user_id, r.movie_id`)
	if err != nil {
		return 0, 0, fmt.Errorf("export: query ratings: %w", err)
	}
	defer rs.Close()
	var (
		line     []byte
		lastUser int64 = -1
	)
	for rs.Next() {
		var uid, movie, ts int64
		var rating float64
		if err := rs.Scan(&uid, &movie, &rating, &ts); err != nil {
			return 0, 0, fmt.Errorf("export: scan: %w", err)
		}
		if rating < 0.5 || rating > 5 || rating*2 != math.Trunc(rating*2) {
			return 0, 0, fmt.Errorf("export: rating out of range for user %d", uid)
		}
		line = line[:0]
		line = strconv.AppendInt(line, uid, 10)
		line = append(line, ',')
		line = strconv.AppendInt(line, movie, 10)
		line = append(line, ',')
		line = strconv.AppendFloat(line, rating, 'f', 1, 64)
		line = append(line, ',')
		line = strconv.AppendInt(line, ts, 10)
		line = append(line, '\n')
		if _, err := w.Write(line); err != nil {
			return 0, 0, err
		}
		rows++
		if uid != lastUser {
			users++
			lastUser = uid
		}
	}
	if err := rs.Err(); err != nil {
		return 0, 0, fmt.Errorf("export: read ratings: %w", err)
	}
	return rows, users, nil
}

// checkDir refuses a directory (or a symlink to one) writable by group or
// others.
func checkDir(dir string) error {
	st, err := os.Stat(dir)
	if err != nil {
		return fmt.Errorf("export: %w", err)
	}
	if !st.IsDir() {
		return fmt.Errorf("export: %s is not a directory", dir)
	}
	if st.Mode().Perm()&0o022 != 0 {
		return fmt.Errorf("export: %s is writable by group or others (mode %o); use 0750 or stricter", dir, st.Mode().Perm())
	}
	return nil
}

// publishNew writes a complete, fsynced temp file and publishes it at path
// with a hard link, which fails if path already exists, so an existing
// export is never replaced. The temp name is then removed.
func publishNew(dir, path string, fill func(*bufio.Writer) error) error {
	tmp, err := writeTemp(dir, fill)
	if err != nil {
		return err
	}
	defer func() { _ = os.Remove(tmp) }()
	if err := os.Link(tmp, path); err != nil {
		return fmt.Errorf("export: publish %s: %w", filepath.Base(path), err)
	}
	if err := os.Remove(tmp); err != nil {
		return err
	}
	return syncDir(dir)
}

// replaceAtomically writes a complete, fsynced temp file and renames it over
// path, so readers see either the old file or the whole new one.
func replaceAtomically(dir, path string, fill func(*bufio.Writer) error) error {
	tmp, err := writeTemp(dir, fill)
	if err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return fmt.Errorf("export: rename into place: %w", err)
	}
	return syncDir(dir)
}

// writeTemp creates a 0640 temp file in dir (recs-train reads exports
// through the group), fills, fsyncs and closes it, and returns its path. On
// failure nothing is left behind.
func writeTemp(dir string, fill func(*bufio.Writer) error) (name string, err error) {
	f, err := os.CreateTemp(dir, ".export-*.tmp")
	if err != nil {
		return "", fmt.Errorf("export: create temp file: %w", err)
	}
	defer func() {
		if err != nil {
			_ = f.Close()
			_ = os.Remove(f.Name())
		}
	}()
	if err = f.Chmod(0o640); err != nil {
		return "", err
	}
	w := bufio.NewWriter(f)
	if err = fill(w); err != nil {
		return "", err
	}
	if err = w.Flush(); err != nil {
		return "", err
	}
	if err = f.Sync(); err != nil {
		return "", err
	}
	if err = f.Close(); err != nil {
		return "", err
	}
	return f.Name(), nil
}

func syncDir(dir string) error {
	d, err := os.Open(dir)
	if err != nil {
		return err
	}
	defer d.Close()
	return d.Sync()
}

// clockSkew is how far in the future an export's name may be dated before
// it is treated as expired (the clock stepped back, or a bad name).
const clockSkew = 24 * time.Hour

// prune applies export retention as of now:
//
//   - kept: the export LATEST names, protect (the one just written, or ""),
//     and, when neither exists, the newest datable export;
//   - otherwise deleted when beyond the Keep newest, at least MaxAge old,
//     dated more than clockSkew in the future, or not datable at all;
//   - this package's temp files older than staleTemp are deleted too.
//
// Names embed a UTC timestamp in a fixed-width format, so name order is
// time order. Only files matching exportName or tempName are ever touched.
func prune(dir string, now time.Time, protect string) error {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	latest := ""
	if b, err := os.ReadFile(filepath.Join(dir, LatestName)); err == nil {
		latest = strings.TrimSpace(string(b))
	}
	var valid, expired []string
	var errs []error
	for _, e := range entries {
		if !e.Type().IsRegular() {
			continue
		}
		name := e.Name()
		switch {
		case exportName.MatchString(name):
			if name == protect || name == latest {
				continue
			}
			at, ok := exportTime(name)
			if !ok || at.Sub(now) > clockSkew {
				expired = append(expired, name)
				continue
			}
			valid = append(valid, name)
		case tempName.MatchString(name):
			if info, err := e.Info(); err == nil && time.Since(info.ModTime()) > staleTemp {
				if err := os.Remove(filepath.Join(dir, name)); err != nil && !errors.Is(err, fs.ErrNotExist) {
					errs = append(errs, err)
				}
			}
		}
	}
	sort.Sort(sort.Reverse(sort.StringSlice(valid))) // newest first
	kept := 0
	if protect != "" {
		kept++
	}
	if latest != "" && latest != protect {
		kept++
	}
	for i, name := range valid {
		at, _ := exportTime(name)
		onlyCopy := i == 0 && kept == 0 // nothing protected: keep the newest
		if onlyCopy || (kept < Keep && now.Sub(at) < MaxAge) {
			kept++
			continue
		}
		expired = append(expired, name)
	}
	for _, name := range expired {
		if err := os.Remove(filepath.Join(dir, name)); err != nil {
			errs = append(errs, err)
		}
	}
	return errors.Join(errs...)
}

// exportTime parses the timestamp in an export's name.
func exportTime(name string) (time.Time, bool) {
	at, err := time.Parse(exportLayout, strings.TrimSuffix(strings.TrimPrefix(name, "ratings-"), ".csv"))
	return at, err == nil
}
