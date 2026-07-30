package updates

import (
	"fmt"
	"net/url"
	"strings"
	"sync"
)

// Baked public origin (plan §6.1 / invariant 7). Shipping binaries must keep
// these paths stable even if the CDN backend moves.
const (
	PublicHost = "6sryusk.com"

	PathBrandRoot  = "/tatsunarisounds/"
	PathUpdatesV1  = "/tatsunarisounds/updates/v1/"
	PathArtifacts  = "/tatsunarisounds/artifacts/"
	PathNotes      = "/tatsunarisounds/notes/"
	PathInstallSH  = "/tatsunarisounds/install.sh"
	PathInstallPS1 = "/tatsunarisounds/install.ps1"

	LatestJSONURL   = "https://6sryusk.com/tatsunarisounds/updates/v1/latest.json"
	CatalogJSONURL  = "https://6sryusk.com/tatsunarisounds/updates/v1/catalog.json"
	HumanUpdatesURL = "https://6sryusk.com/tatsunarisounds/updates/"
	BrandHomeURL    = "https://6sryusk.com/tatsunarisounds/"
)

// URLKind selects which path prefix a field is allowed to reference.
type URLKind int

const (
	// URLAnyAllowed accepts any of the brand path prefixes (legacy helpers).
	URLAnyAllowed URLKind = iota
	// URLArtifact is for downloadable plugin/installer binaries.
	URLArtifact
	// URLNotes is for changelogUrl fields.
	URLNotes
	// URLFeed is for update feed documents themselves.
	URLFeed
)

var (
	hostsMu      sync.RWMutex
	allowedHosts = []string{PublicHost} // baked-in production default
)

// SetAllowedHosts replaces the host allowlist. Pass nil/empty to fail-closed
// (tests only). Production code should leave the baked-in PublicHost.
func SetAllowedHosts(hosts []string) {
	hostsMu.Lock()
	defer hostsMu.Unlock()
	allowedHosts = append([]string(nil), hosts...)
}

// AllowedHostsSnapshot returns a copy of the current host allowlist.
func AllowedHostsSnapshot() []string {
	hostsMu.RLock()
	defer hostsMu.RUnlock()
	return append([]string(nil), allowedHosts...)
}

// CheckHTTPSURL validates host allowlist only (any brand path). Prefer CheckURL
// with an explicit URLKind for catalog fields.
func CheckHTTPSURL(raw string) error {
	return CheckURL(URLAnyAllowed, raw)
}

// CheckURL validates scheme, host allowlist, and the path prefix required for kind.
// Redirect targets must be re-checked by the transport layer with the same kind.
func CheckURL(kind URLKind, raw string) error {
	if raw == "" {
		return fmt.Errorf("url is empty")
	}
	u, err := url.Parse(raw)
	if err != nil {
		return fmt.Errorf("parse url: %w", err)
	}
	if u.Scheme != "https" {
		return fmt.Errorf("url scheme must be https, got %q", u.Scheme)
	}
	if u.Host == "" {
		return fmt.Errorf("url host is empty")
	}
	host := strings.ToLower(u.Hostname())
	hostsMu.RLock()
	hosts := append([]string(nil), allowedHosts...)
	hostsMu.RUnlock()
	if len(hosts) == 0 {
		return fmt.Errorf("url host allowlist is empty (domain not configured)")
	}
	okHost := false
	for _, h := range hosts {
		if strings.EqualFold(h, host) {
			okHost = true
			break
		}
	}
	if !okHost {
		return fmt.Errorf("url host %q is not on the allowlist", host)
	}

	path := u.EscapedPath()
	if path == "" {
		path = "/"
	}
	prefix, err := prefixForKind(kind)
	if err != nil {
		return err
	}
	if kind == URLAnyAllowed {
		if !hasAnyBrandPrefix(path) {
			return fmt.Errorf("url path %q is outside /tatsunarisounds/ allowlist", path)
		}
		return nil
	}
	if !strings.HasPrefix(path, prefix) {
		return fmt.Errorf("url path %q must start with %q", path, prefix)
	}
	return nil
}

func prefixForKind(kind URLKind) (string, error) {
	switch kind {
	case URLAnyAllowed:
		return PathBrandRoot, nil
	case URLArtifact:
		return PathArtifacts, nil
	case URLNotes:
		return PathNotes, nil
	case URLFeed:
		return PathUpdatesV1, nil
	default:
		return "", fmt.Errorf("unknown url kind %d", kind)
	}
}

func hasAnyBrandPrefix(path string) bool {
	for _, p := range []string{PathUpdatesV1, PathArtifacts, PathNotes, PathBrandRoot} {
		if strings.HasPrefix(path, p) {
			return true
		}
	}
	// Exact brand scripts
	if path == PathInstallSH || path == PathInstallPS1 {
		return true
	}
	return false
}
