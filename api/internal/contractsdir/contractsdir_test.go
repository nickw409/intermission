package contractsdir

import (
	"os"
	"path/filepath"
	"testing"
)

func TestFindWalksUp(t *testing.T) {
	t.Setenv(EnvVar, "")
	dir, err := Find()
	if err != nil {
		t.Fatalf("Find: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "model.yaml")); err != nil {
		t.Fatalf("model.yaml missing in %s: %v", dir, err)
	}
}

func TestFindEnvOverride(t *testing.T) {
	tmp := t.TempDir()
	t.Setenv(EnvVar, tmp)
	got, err := Find()
	if err != nil || got != tmp {
		t.Fatalf("Find() = %q, %v; want %q", got, err, tmp)
	}
	t.Setenv(EnvVar, filepath.Join(tmp, "missing"))
	if _, err := Find(); err == nil {
		t.Fatal("Find with a missing override dir: want error")
	}
}
