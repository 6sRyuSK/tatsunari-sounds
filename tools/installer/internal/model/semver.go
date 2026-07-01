package model

import (
	"fmt"
	"strconv"
	"strings"
)

// SemVer is a plain MAJOR.MINOR.PATCH version. Plugin versions in this repo are
// always three dotted integers (e.g. "0.2.1"); we don't need pre-release or
// build metadata, so a 3-int compare is exact and pulls in no dependency.
type SemVer struct {
	Major, Minor, Patch int
}

// ParseSemVer parses "0.2.1" (an optional leading "v" is tolerated).
func ParseSemVer(s string) (SemVer, error) {
	s = strings.TrimSpace(s)
	s = strings.TrimPrefix(s, "v")
	parts := strings.Split(s, ".")
	if len(parts) != 3 {
		return SemVer{}, fmt.Errorf("invalid semver %q: want MAJOR.MINOR.PATCH", s)
	}
	var out SemVer
	for i, p := range parts {
		n, err := strconv.Atoi(p)
		if err != nil || n < 0 {
			return SemVer{}, fmt.Errorf("invalid semver %q: bad component %q", s, p)
		}
		switch i {
		case 0:
			out.Major = n
		case 1:
			out.Minor = n
		case 2:
			out.Patch = n
		}
	}
	return out, nil
}

// CompareSemVer returns -1 if a<b, 0 if a==b, +1 if a>b.
func CompareSemVer(a, b SemVer) int {
	switch {
	case a.Major != b.Major:
		return cmpInt(a.Major, b.Major)
	case a.Minor != b.Minor:
		return cmpInt(a.Minor, b.Minor)
	default:
		return cmpInt(a.Patch, b.Patch)
	}
}

func cmpInt(a, b int) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	default:
		return 0
	}
}

// StateFor computes an InstallState from an installed version string and the
// latest available version. An empty installed string means "not installed".
// A non-empty-but-unparseable installed string means "installed, version
// unknown" (e.g. a receipt-less manual install probed on disk).
func StateFor(installed, latest string) InstallState {
	if installed == "" {
		return StateNotInstalled
	}
	iv, ierr := ParseSemVer(installed)
	lv, lerr := ParseSemVer(latest)
	if ierr != nil || lerr != nil {
		return StateInstalledUnknown
	}
	if CompareSemVer(iv, lv) < 0 {
		return StateUpdateAvailable
	}
	return StateUpToDate
}
