// Package contractsdir locates the repository's contracts/ directory for tests
// and test fakes.
//
// Owner: spine (orchestrator). Resolution order: the RECS_CONTRACTS_DIR
// environment variable, then walking up from the current working directory
// until a directory named "contracts" containing model.yaml is found.
package contractsdir

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

// EnvVar overrides the contracts directory location.
const EnvVar = "RECS_CONTRACTS_DIR"

// ErrNotFound is returned when no contracts directory can be located.
var ErrNotFound = errors.New("contracts directory not found")

// Find returns the absolute path of the contracts directory.
func Find() (string, error) {
	if dir := os.Getenv(EnvVar); dir != "" {
		abs, err := filepath.Abs(dir)
		if err != nil {
			return "", err
		}
		if !isDir(abs) {
			return "", fmt.Errorf("%s=%q is not a directory: %w", EnvVar, dir, ErrNotFound)
		}
		return abs, nil
	}
	wd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for dir := wd; ; {
		cand := filepath.Join(dir, "contracts")
		if isFile(filepath.Join(cand, "model.yaml")) {
			return cand, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", fmt.Errorf("searched upward from %s: %w (set %s)", wd, ErrNotFound, EnvVar)
		}
		dir = parent
	}
}

// Fixtures returns the path of contracts/fixtures/<kind> (kind is "model",
// "public" or "reports"). The directory may not exist yet.
func Fixtures(kind string) (string, error) {
	dir, err := Find()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "fixtures", kind), nil
}

func isDir(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.IsDir()
}

func isFile(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.Mode().IsRegular()
}
