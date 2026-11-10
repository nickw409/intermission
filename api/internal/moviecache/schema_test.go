package moviecache_test

import (
	"strconv"
	"strings"
	"testing"

	"github.com/nickw409/intermission/api/internal/moviecache/cachetest"
)

// Migration 0002: the movies table's columns and constraints, checked
// directly against the migrated schema.
func TestMoviesTableSchema(t *testing.T) {
	d := cachetest.OpenDB(t)
	rows, err := d.Reader.Query(`SELECT name, type, "notnull", coalesce(dflt_value, ''), pk FROM pragma_table_info('movies')`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var got []string
	for rows.Next() {
		var name, typ, dflt string
		var notnull, pk int
		if err := rows.Scan(&name, &typ, &notnull, &dflt, &pk); err != nil {
			t.Fatal(err)
		}
		got = append(got, strings.Join([]string{name, typ, strconv.Itoa(notnull), dflt, strconv.Itoa(pk)}, " "))
	}
	want := []string{
		"movie_id INTEGER 0  1", "title TEXT 1  0", "year INTEGER 0  0", "genres_json TEXT 1  0",
		"tmdb_id INTEGER 0  0", "rating_count INTEGER 1  0", "mean_rating REAL 0  0", "updated_at INTEGER 1  0",
	}
	if strings.Join(got, "|") != strings.Join(want, "|") {
		t.Fatalf("movies columns:\n got %q\nwant %q", got, want)
	}

	insert := "INSERT INTO movies (movie_id, title, year, genres_json, tmdb_id, rating_count, mean_rating, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, 0)"
	ok := func(args ...any) {
		t.Helper()
		if _, err := d.Writer.Exec(insert, args...); err != nil {
			t.Errorf("valid row %v rejected: %v", args, err)
		}
	}
	bad := func(why string, args ...any) {
		t.Helper()
		if _, err := d.Writer.Exec(insert, args...); err == nil {
			t.Errorf("%s accepted: %v", why, args)
		}
	}
	ok(1, "One", 1995, `["Comedy"]`, 862, 10, 3.9)
	ok(2147483647, "Max", nil, `[]`, nil, 0, nil)
	bad("duplicate movie_id", 1, "Again", 1995, `[]`, nil, 0, nil)
	bad("movie_id 0", 0, "Zero", nil, `[]`, nil, 0, nil)
	bad("movie_id > 2^31-1", int64(2147483648), "Big", nil, `[]`, nil, 0, nil)
	bad("NULL title", 3, nil, nil, `[]`, nil, 0, nil)
	bad("invalid genres JSON", 4, "G", nil, `[nope`, nil, 0, nil)
	bad("genres JSON object", 5, "G", nil, `{"a":1}`, nil, 0, nil)
	bad("NULL genres", 6, "G", nil, nil, nil, 0, nil)
	bad("negative rating_count", 7, "C", nil, `[]`, nil, -1, nil)
	bad("NULL rating_count", 8, "C", nil, `[]`, nil, nil, nil)
}
