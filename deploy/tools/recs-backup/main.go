// Command recs-backup makes, checks and restores online backups of recs-api's SQLite
// database (spec: "Backups and recovery"). It runs as a one-shot container (compose service
// `backup`, profile `jobs`) so the backup needs no shell, no sqlite3 CLI and no host tools.
//
// Usage:
//
//	recs-backup backup  --db /srv/data/db/app.db --dir /srv/data/backups [--keep-daily 7] [--keep-weekly 4]
//	recs-backup verify  --file /srv/data/backups/daily/app-2026-11-10.db
//	recs-backup latest  --dir /srv/data/backups
//	recs-backup restore --from <backup file> --db /srv/data/db/app.db [--force]
//
// How a backup is made:
//
//  1. `VACUUM INTO` writes a consistent, compacted copy of the live database to a temporary
//     file. It reads inside one transaction, so recs-api can keep serving (WAL mode) while it
//     runs, and the copy never contains a half-applied write.
//  2. `PRAGMA integrity_check` must return exactly "ok" on the copy, or the copy is deleted
//     and the command fails.
//  3. The copy is made 0600, fsynced and renamed to daily/app-YYYY-MM-DD.db (re-running on the
//     same day replaces that day's file, so the job is idempotent).
//  4. If the newest weekly copy is 7 or more days old (or there is none), the daily file is
//     also copied to weekly/. Missing a Sunday therefore never skips a week.
//  5. Old files are pruned: the newest --keep-daily daily and --keep-weekly weekly copies stay,
//     except that any copy older than maxAge (35 days) is deleted whatever the counts say,
//     because the site promises that backups holding a deleted account's data are gone
//     within 5 weeks. The newest copy in each directory is never deleted.
//
// The tool never logs row data; it prints one JSON line per command.
package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	_ "modernc.org/sqlite"
)

const usage = `usage:
  recs-backup backup  --db PATH --dir DIR [--keep-daily 7] [--keep-weekly 4]
  recs-backup verify  --file PATH
  recs-backup latest  --dir DIR
  recs-backup restore --from PATH --db PATH [--force]
`

const (
	filePrefix = "app-"
	fileSuffix = ".db"
	dateLayout = "2006-01-02"
)

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Stdout, os.Stderr, time.Now))
}

func run(ctx context.Context, args []string, stdout, stderr io.Writer, now func() time.Time) int {
	if len(args) == 0 {
		fmt.Fprint(stderr, usage)
		return 2
	}
	fs := flag.NewFlagSet(args[0], flag.ContinueOnError)
	fs.SetOutput(stderr)
	var (
		dbPath     = fs.String("db", "", "SQLite database file")
		dir        = fs.String("dir", "", "backup directory (holds daily/ and weekly/)")
		file       = fs.String("file", "", "backup file to verify")
		from       = fs.String("from", "", "backup file to restore")
		keepDaily  = fs.Int("keep-daily", 7, "daily copies to keep")
		keepWeekly = fs.Int("keep-weekly", 4, "weekly copies to keep")
		force      = fs.Bool("force", false, "restore over an existing database")
	)
	if err := fs.Parse(args[1:]); err != nil {
		return 2
	}

	var (
		result any
		err    error
	)
	switch args[0] {
	case "backup":
		if *dbPath == "" || *dir == "" || *keepDaily < 1 || *keepWeekly < 1 {
			fmt.Fprint(stderr, usage)
			return 2
		}
		result, err = backup(ctx, *dbPath, *dir, *keepDaily, *keepWeekly, now().UTC())
	case "verify":
		if *file == "" {
			fmt.Fprint(stderr, usage)
			return 2
		}
		err = integrityCheck(ctx, *file)
		result = map[string]any{"verified": *file}
	case "latest":
		if *dir == "" {
			fmt.Fprint(stderr, usage)
			return 2
		}
		var p string
		p, err = latest(*dir)
		result = map[string]any{"latest": p}
	case "restore":
		if *from == "" || *dbPath == "" {
			fmt.Fprint(stderr, usage)
			return 2
		}
		err = restore(ctx, *from, *dbPath, *force)
		result = map[string]any{"restored": *from, "db": *dbPath}
	default:
		fmt.Fprintf(stderr, "unknown command %q\n\n%s", args[0], usage)
		return 2
	}
	if err != nil {
		writeJSON(stderr, map[string]any{"command": args[0], "ok": false, "error": err.Error()})
		return 1
	}
	writeJSON(stdout, map[string]any{"command": args[0], "ok": true, "result": result})
	return 0
}

func writeJSON(w io.Writer, v any) {
	b, _ := json.Marshal(v)
	fmt.Fprintln(w, string(b))
}

// BackupResult is printed after a successful backup.
type BackupResult struct {
	Daily   string   `json:"daily"`
	Weekly  string   `json:"weekly,omitempty"`
	Bytes   int64    `json:"bytes"`
	Removed []string `json:"removed,omitempty"`
}

func backup(ctx context.Context, dbPath, dir string, keepDaily, keepWeekly int, now time.Time) (*BackupResult, error) {
	if _, err := os.Stat(dbPath); err != nil {
		return nil, fmt.Errorf("database: %w", err)
	}
	dailyDir, weeklyDir := filepath.Join(dir, "daily"), filepath.Join(dir, "weekly")
	for _, d := range []string{dailyDir, weeklyDir} {
		if err := os.MkdirAll(d, 0o700); err != nil {
			return nil, err
		}
	}
	name := filePrefix + now.Format(dateLayout) + fileSuffix
	final := filepath.Join(dailyDir, name)
	tmp := final + ".tmp"
	_ = os.Remove(tmp) // left over from a crashed run

	if err := vacuumInto(ctx, dbPath, tmp); err != nil {
		_ = os.Remove(tmp)
		return nil, err
	}
	if err := integrityCheck(ctx, tmp); err != nil {
		_ = os.Remove(tmp)
		return nil, fmt.Errorf("backup copy failed its integrity check: %w", err)
	}
	if err := finalize(tmp, final); err != nil {
		_ = os.Remove(tmp)
		return nil, err
	}
	st, err := os.Stat(final)
	if err != nil {
		return nil, err
	}
	res := &BackupResult{Daily: final, Bytes: st.Size()}

	weekly, err := listBackups(weeklyDir)
	if err != nil {
		return nil, err
	}
	if needWeekly(weekly, now) {
		w := filepath.Join(weeklyDir, name)
		if err := copyFile(final, w+".tmp"); err != nil {
			return nil, err
		}
		if err := finalize(w+".tmp", w); err != nil {
			return nil, err
		}
		res.Weekly = w
	}

	for _, d := range []struct {
		dir  string
		keep int
	}{{dailyDir, keepDaily}, {weeklyDir, keepWeekly}} {
		removed, err := prune(d.dir, d.keep, now)
		if err != nil {
			return nil, err
		}
		res.Removed = append(res.Removed, removed...)
	}
	return res, nil
}

// vacuumInto writes a consistent copy of dbPath to dest with VACUUM INTO.
func vacuumInto(ctx context.Context, dbPath, dest string) error {
	db, err := openDB(dbPath, true)
	if err != nil {
		return err
	}
	defer db.Close()
	if _, err := db.ExecContext(ctx, "VACUUM INTO ?", dest); err != nil {
		return fmt.Errorf("VACUUM INTO: %w", err)
	}
	return nil
}

// integrityCheck opens path read-only and requires PRAGMA integrity_check to return "ok".
func integrityCheck(ctx context.Context, path string) error {
	if _, err := os.Stat(path); err != nil {
		return err
	}
	db, err := openDB(path, true)
	if err != nil {
		return err
	}
	defer db.Close()
	rows, err := db.QueryContext(ctx, "PRAGMA integrity_check")
	if err != nil {
		return fmt.Errorf("integrity_check: %w", err)
	}
	defer rows.Close()
	var msgs []string
	for rows.Next() {
		var m string
		if err := rows.Scan(&m); err != nil {
			return err
		}
		msgs = append(msgs, m)
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("integrity_check: %w", err)
	}
	if len(msgs) != 1 || msgs[0] != "ok" {
		if len(msgs) > 5 {
			msgs = msgs[:5]
		}
		return fmt.Errorf("integrity_check: %s", strings.Join(msgs, "; "))
	}
	return nil
}

func openDB(path string, readOnly bool) (*sql.DB, error) {
	q := "?_pragma=busy_timeout(10000)"
	if readOnly {
		q += "&mode=ro"
	}
	db, err := sql.Open("sqlite", "file:"+path+q)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	return db, nil
}

// latest returns the newest backup file under dir (daily or weekly).
func latest(dir string) (string, error) {
	var all []string
	for _, sub := range []string{"daily", "weekly"} {
		files, err := listBackups(filepath.Join(dir, sub))
		if err != nil {
			return "", err
		}
		all = append(all, files...)
	}
	if len(all) == 0 {
		return "", fmt.Errorf("no backups under %s", dir)
	}
	sort.Slice(all, func(i, j int) bool { return filepath.Base(all[i]) < filepath.Base(all[j]) })
	return all[len(all)-1], nil
}

// restore copies a verified backup to dbPath. recs-api must be stopped first.
func restore(ctx context.Context, from, dbPath string, force bool) error {
	if err := integrityCheck(ctx, from); err != nil {
		return fmt.Errorf("refusing to restore %s: %w", from, err)
	}
	if _, err := os.Stat(dbPath); err == nil && !force {
		return fmt.Errorf("%s exists; stop recs-api and pass --force to replace it", dbPath)
	} else if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	tmp := dbPath + ".restore-tmp"
	if err := copyFile(from, tmp); err != nil {
		return err
	}
	// Stale WAL/SHM files belong to the database being replaced; SQLite would replay them
	// over the restored copy.
	for _, ext := range []string{"-wal", "-shm"} {
		if err := os.Remove(dbPath + ext); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
	}
	if err := finalize(tmp, dbPath); err != nil {
		return err
	}
	return integrityCheck(ctx, dbPath)
}

// finalize makes tmp 0600, fsyncs it, renames it to final and fsyncs the directory.
func finalize(tmp, final string) error {
	if err := os.Chmod(tmp, 0o600); err != nil {
		return err
	}
	f, err := os.Open(tmp)
	if err != nil {
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmp, final); err != nil {
		return err
	}
	d, err := os.Open(filepath.Dir(final))
	if err != nil {
		return err
	}
	defer d.Close()
	return d.Sync()
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

// listBackups returns the backup files in dir, oldest first (names sort by date).
func listBackups(dir string) ([]string, error) {
	entries, err := os.ReadDir(dir)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var out []string
	for _, e := range entries {
		if _, ok := backupDate(e.Name()); ok && e.Type().IsRegular() {
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	sort.Strings(out)
	return out, nil
}

// backupDate parses app-YYYY-MM-DD.db.
func backupDate(name string) (time.Time, bool) {
	if !strings.HasPrefix(name, filePrefix) || !strings.HasSuffix(name, fileSuffix) {
		return time.Time{}, false
	}
	t, err := time.Parse(dateLayout, strings.TrimSuffix(strings.TrimPrefix(name, filePrefix), fileSuffix))
	return t, err == nil
}

// needWeekly reports whether a weekly copy is due: none exists, or the newest is ≥ 7 days old.
func needWeekly(weekly []string, now time.Time) bool {
	if len(weekly) == 0 {
		return true
	}
	newest, _ := backupDate(filepath.Base(weekly[len(weekly)-1]))
	today, _ := time.Parse(dateLayout, now.Format(dateLayout))
	return today.Sub(newest) >= 7*24*time.Hour
}

// maxAge is the oldest a backup may be (by the date in its name) before prune deletes it
// regardless of keep.
const maxAge = 35 * 24 * time.Hour

// prune deletes the backups in dir beyond the newest keep, and any older than maxAge as of
// now, but never the newest one. It returns what it removed.
func prune(dir string, keep int, now time.Time) ([]string, error) {
	files, err := listBackups(dir) // oldest first
	if err != nil {
		return nil, err
	}
	today, _ := time.Parse(dateLayout, now.UTC().Format(dateLayout))
	var removed []string
	for i, f := range files[:max(len(files)-1, 0)] {
		date, _ := backupDate(filepath.Base(f))
		if len(files)-i <= keep && today.Sub(date) <= maxAge {
			continue
		}
		if err := os.Remove(f); err != nil {
			return removed, err
		}
		removed = append(removed, f)
	}
	return removed, nil
}
