package modelclient

import (
	"encoding/json"
	"reflect"
	"testing"
)

// FuzzDecodeStrict: every response type decodes arbitrary bytes without
// panicking, and anything accepted survives a re-encode/decode round trip.
func FuzzDecodeStrict(f *testing.F) {
	for _, s := range []string{
		`{"movie_ids":[1,2,3]}`,
		`{"movies":[{"movie_id":1,"title":"t","year":null,"genres":[],"tmdb_id":null,"rating_count":0,"mean_rating":null}],"missing":[2]}`,
		`{"model":"knn","movie_id":1,"items":[{"movie_id":2,"similarity":0.5}]}`,
		`{"artifact_version":"v","trained_at":"2026-11-09T21:14:03Z","default_model":"als","models":["pop"]}`,
		`{"model":"als","items":[{"movie_id":1,"score":1,"because":[{"movie_id":2,"weight":0.1}]}],"ignored":0}`,
		`{"results":[],"ignored":0}`, `{"artifact_version":"v"}`, `{} {}`, `{"x":1}`, `[`, ``,
	} {
		f.Add([]byte(s))
	}
	targets := []func() any{
		func() any { return new(IDList) }, func() any { return new(BatchResponse) }, func() any { return new(SimilarResponse) },
		func() any { return new(ModelsResponse) }, func() any { return new(RecommendResponse) }, func() any { return new(CompareResponse) },
		func() any { return new(VersionResponse) }, func() any { return new(ErrorBody) },
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		for _, mk := range targets {
			v := mk()
			if decodeStrict(data, v) != nil {
				continue
			}
			again, err := json.Marshal(v)
			if err != nil {
				t.Fatalf("re-encode %T: %v", v, err)
			}
			w := mk()
			if err := decodeStrict(again, w); err != nil || !reflect.DeepEqual(v, w) {
				t.Fatalf("%T round trip: %v\n%#v\n%#v", v, err, v, w)
			}
		}
	})
}
