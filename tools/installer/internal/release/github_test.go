package release

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestFetchLatestRelease(t *testing.T) {
	const body = `[
		{"tag_name":"2026.1","assets":[{"name":"manifest.json","browser_download_url":"https://x/1","size":1}]},
		{"tag_name":"2026.2","assets":[{"name":"manifest.json","browser_download_url":"https://x/2","size":2}]},
		{"tag_name":"2025.9","assets":[]},
		{"tag_name":"dynamic-eq-v0.4.0","assets":[]},
		{"tag_name":"2026.2","draft":true,"assets":[]}
	]`
	// A TLS test server so the client's HTTPS-only enforcement stays exercised;
	// srv.Client() trusts the server's self-signed cert.
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("User-Agent") == "" {
			t.Error("missing User-Agent header")
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(body))
	}))
	defer srv.Close()

	c := NewClient("6sRyuSK", "tatsunari-plugins", "")
	c.HTTP = srv.Client()
	c.BaseURL = srv.URL

	rel, err := c.FetchLatestRelease(context.Background())
	if err != nil {
		t.Fatalf("FetchLatestRelease: %v", err)
	}
	if rel.Tag != "2026.2" {
		t.Errorf("picked tag %q, want 2026.2 (highest year.n, non-draft)", rel.Tag)
	}
	if _, ok := rel.Asset("manifest.json"); !ok {
		t.Error("expected manifest.json asset on chosen release")
	}
}

func TestRequireHTTPS(t *testing.T) {
	if err := requireHTTPS("http://example.com/x"); err == nil {
		t.Error("http url should be rejected")
	}
	if err := requireHTTPS("https://example.com/x"); err != nil {
		t.Errorf("https url should pass: %v", err)
	}
}
