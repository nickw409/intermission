# recs-api build and quality gates. `make check` is what CI runs.

GO           ?= go
STATICCHECK  := honnef.co/go/tools/cmd/staticcheck@v0.8.2
GOVULNCHECK  := golang.org/x/vuln/cmd/govulncheck@v1.9.0
FUZZTIME     ?= 10s
BIN          ?= bin/recs-api

.PHONY: build test vet staticcheck vulncheck fuzz-smoke check clean

build:
	$(GO) build -trimpath -o $(BIN) ./cmd/recs-api

test:
	$(GO) test -race ./...

vet:
	$(GO) vet ./...

staticcheck:
	$(GO) run $(STATICCHECK) ./...

vulncheck:
	$(GO) run $(GOVULNCHECK) ./...

# Runs every Fuzz* target for FUZZTIME each (a smoke run, not the long
# 15-minute runs that are scheduled separately).
fuzz-smoke:
	@set -e; for pkg in $$($(GO) list ./...); do \
	  for fn in $$($(GO) test -list '^Fuzz' $$pkg | grep '^Fuzz' || true); do \
	    echo "fuzz $$pkg $$fn"; \
	    $(GO) test -run '^$$' -fuzz "^$$fn$$" -fuzztime $(FUZZTIME) $$pkg; \
	  done; \
	done

check: vet staticcheck vulncheck test

clean:
	rm -rf bin
