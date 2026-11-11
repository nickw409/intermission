package main

import "testing"

// The -genres flag is appended to every recommend request as &genres=,
// and left out entirely when empty (the unfiltered gate run).
func TestRecommendPath(t *testing.T) {
	cases := []struct {
		genres, want string
	}{
		{"", "/api/recommend?model=knn&k=20"},
		{"Film-Noir", "/api/recommend?model=knn&k=20&genres=Film-Noir"},
		{"Horror,Thriller", "/api/recommend?model=knn&k=20&genres=Horror%2CThriller"},
	}
	for _, c := range cases {
		if got := recommendPath(config{model: "knn", k: 20, genres: c.genres}); got != c.want {
			t.Errorf("genres %q: %s, want %s", c.genres, got, c.want)
		}
	}
}

// Malformed lists are refused before any setup, so a typo can't turn into a
// run of 400s after several minutes of signups.
func TestCheckGenres(t *testing.T) {
	for _, ok := range []string{"", "Film-Noir", "Action,Adventure,Animation,Children,Comedy,Crime,Documentary,Drama,Fantasy,Film-Noir,Horror,IMAX,Musical,Mystery,Romance,Sci-Fi,Thriller,War,Western"} {
		if err := checkGenres(ok); err != nil {
			t.Errorf("%q: %v", ok, err)
		}
	}
	for _, bad := range []string{"Horror,", "Horror Thriller", "Horror,,Thriller", "horror;x", ","} {
		if err := checkGenres(bad); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
}
