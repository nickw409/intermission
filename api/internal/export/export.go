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
	"time"

	"github.com/nickw409/intermission/api/internal/app"
	"github.com/nickw409/intermission/api/internal/db"
)

// The file format is contracts/export.md (format_version 1).

// Header is the exact first line of every export.
const Header = "site_user_id,movie_id,rating,timestamp"

// Keep is how many exports are retained.
const Keep = 7

// LatestName is the pointer file holding the newest export's file name.
const LatestName = "LATEST"

// exportName matches the files this package writes (and may delete).
var exportName = regexp.MustCompile(`^ratings-\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z\.csv$`)

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
	res, err := Export(ctx, d.DB, *dir, time.Now())
	if err != nil {
		return err
	}
	logger := d.Logger
	if logger == nil {
		logger = slog.Default()
	}
	logger.Info("ratings exported", "file", filepath.Base(res.Path), "rows", res.Rows, "users", res.Users)
	return nil
}

// Export writes every rating of every existing account to
// dir/ratings-<UTC time>.csv (temp file, fsync, rename), points dir/LATEST
// at it, and deletes all but the Keep newest exports. Only internal user IDs
// are written, never usernames. An existing file of the same name is never
// overwritten (an error is returned instead).
func Export(ctx context.Context, database *db.DB, dir string, now time.Time) (Result, error) {
	if err := os.MkdirAll(dir, 0o750); err != nil {
		return Result{}, fmt.Errorf("export: create %s: %w", dir, err)
	}
	name := "ratings-" + now.UTC().Format("2006-01-02T15-04-05Z") + ".csv"
	final := filepath.Join(dir, name)
	if _, err := os.Lstat(final); err == nil {
		return Result{}, fmt.Errorf("export: %s already exists", name)
	} else if !errors.Is(err, fs.ErrNotExist) {
		return Result{}, err
	}

	res := Result{Path: final}
	err := writeAtomically(dir, final, func(w *bufio.Writer) error {
		var err error
		res.Rows, res.Users, err = writeRows(ctx, database, w)
		return err
	})
	if err != nil {
		return Result{}, err
	}
	if err := writeAtomically(dir, filepath.Join(dir, LatestName), func(w *bufio.Writer) error {
		_, err := w.WriteString(name)
		return err
	}); err != nil {
		return Result{}, err
	}
	if err := prune(dir); err != nil {
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

// writeAtomically writes a temp file in dir with fill, fsyncs it, renames it
// to path and fsyncs dir, so readers see either the old file or the whole
// new one. Files are 0640 (recs-train reads them through the group).
func writeAtomically(dir, path string, fill func(*bufio.Writer) error) (err error) {
	tmp, err := os.CreateTemp(dir, ".export-*.tmp")
	if err != nil {
		return fmt.Errorf("export: create temp file: %w", err)
	}
	defer func() {
		if err != nil {
			_ = tmp.Close()
			_ = os.Remove(tmp.Name())
		}
	}()
	if err = tmp.Chmod(0o640); err != nil {
		return err
	}
	w := bufio.NewWriter(tmp)
	if err = fill(w); err != nil {
		return err
	}
	if err = w.Flush(); err != nil {
		return err
	}
	if err = tmp.Sync(); err != nil {
		return err
	}
	if err = tmp.Close(); err != nil {
		return err
	}
	if err = os.Rename(tmp.Name(), path); err != nil {
		return fmt.Errorf("export: rename into place: %w", err)
	}
	return syncDir(dir)
}

func syncDir(dir string) error {
	d, err := os.Open(dir)
	if err != nil {
		return err
	}
	defer d.Close()
	return d.Sync()
}

// prune deletes all but the Keep newest exports. Names embed a UTC
// timestamp in a fixed-width format, so name order is time order. Only
// files matching exportName are ever considered.
func prune(dir string) error {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	var exports []string
	for _, e := range entries {
		if e.Type().IsRegular() && exportName.MatchString(e.Name()) {
			exports = append(exports, e.Name())
		}
	}
	sort.Strings(exports)
	var errs []error
	for len(exports) > Keep {
		if err := os.Remove(filepath.Join(dir, exports[0])); err != nil {
			errs = append(errs, err)
		}
		exports = exports[1:]
	}
	return errors.Join(errs...)
}
