package updates

import (
	"fmt"
	"net/url"
	"strings"
	"sync"
)

// AllowedHosts is the HTTPS host allowlist for catalog/latest/artifact URLs
// (design invariant 7). Production binaries must bake a real owned domain here;
// until §6.1 is decided this stays empty and URL checks fail closed except in
// tests that call SetAllowedHosts.
var (
	hostsMu      sync.RWMutex
	allowedHosts []string
)

// SetAllowedHosts replaces the allowlist. Pass nil/empty to fail-closed.
// Tests use this with fixture hosts such as "cdn.example.test".
func SetAllowedHosts(hosts []string) {
	hostsMu.Lock()
	defer hostsMu.Unlock()
	allowedHosts = append([]string(nil), hosts...)
}

// AllowedHostsSnapshot returns a copy of the current allowlist.
func AllowedHostsSnapshot() []string {
	hostsMu.RLock()
	defer hostsMu.RUnlock()
	return append([]string(nil), allowedHosts...)
}

// CheckHTTPSURL validates that raw is an absolute https URL whose host is on
// the allowlist. Redirect targets must be re-checked by the transport layer.
func CheckHTTPSURL(raw string) error {
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
	defer hostsMu.RUnlock()
	if len(allowedHosts) == 0 {
		return fmt.Errorf("url host allowlist is empty (domain not configured)")
	}
	for _, h := range allowedHosts {
		if strings.EqualFold(h, host) {
			return nil
		}
	}
	return fmt.Errorf("url host %q is not on the allowlist", host)
}
