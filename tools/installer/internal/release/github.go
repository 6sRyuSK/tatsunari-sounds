// Package release discovers the latest consolidated GitHub Release and parses
// the artifacts the installer consumes: manifest.json (slug->version), the
// per-plugin asset matrix, the optional catalog.json (display metadata) and
// SHA256SUMS.txt (integrity). All network I/O is HTTPS-only.
package release

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

// DefaultOwner / DefaultRepo are the repository this installer ships for.
const (
	DefaultOwner = "6sRyuSK"
	DefaultRepo  = "tatsunari-plugins"
)

// releaseTagRe matches a consolidated-release tag like "2026.2" (year.n), so we
// ignore per-plugin git tags such as "dynamic-eq-v0.4.0".
var releaseTagRe = regexp.MustCompile(`^(\d{4})\.(\d+)$`)

// Client talks to the GitHub REST API for one repo. baseURL is overridable so
// tests can point it at an httptest server.
type Client struct {
	HTTP    *http.Client
	Owner   string
	Repo    string
	BaseURL string // default https://api.github.com
	Token   string // optional; raises the anonymous rate limit
}

// NewClient returns a Client with sane defaults and HTTPS-only redirects.
func NewClient(owner, repo, token string) *Client {
	return &Client{
		HTTP: &http.Client{
			Timeout: 60 * time.Second,
			CheckRedirect: func(req *http.Request, via []*http.Request) error {
				if req.URL.Scheme != "https" {
					return fmt.Errorf("refusing non-https redirect to %s", req.URL.Redacted())
				}
				if len(via) >= 10 {
					return fmt.Errorf("stopped after 10 redirects")
				}
				return nil
			},
		},
		Owner:   owner,
		Repo:    repo,
		BaseURL: "https://api.github.com",
		Token:   token,
	}
}

// Release is one GitHub release reduced to what the installer needs.
type Release struct {
	Tag    string
	Assets []model.Asset
}

// Asset returns the named asset (exact match) if present.
func (r *Release) Asset(name string) (model.Asset, bool) {
	for _, a := range r.Assets {
		if a.Name == name {
			return a, true
		}
	}
	return model.Asset{}, false
}

// apiRelease mirrors the subset of the GitHub releases payload we read.
type apiRelease struct {
	TagName    string `json:"tag_name"`
	Draft      bool   `json:"draft"`
	Prerelease bool   `json:"prerelease"`
	Assets     []struct {
		Name string `json:"name"`
		URL  string `json:"browser_download_url"`
		Size int64  `json:"size"`
	} `json:"assets"`
}

// FetchLatestRelease lists releases and returns the highest year.n tag.
func (c *Client) FetchLatestRelease(ctx context.Context) (*Release, error) {
	u := fmt.Sprintf("%s/repos/%s/%s/releases?per_page=100", c.BaseURL, c.Owner, c.Repo)
	body, err := c.getJSON(ctx, u)
	if err != nil {
		return nil, err
	}
	defer body.Close()

	var raw []apiRelease
	if err := json.NewDecoder(body).Decode(&raw); err != nil {
		return nil, fmt.Errorf("decode releases: %w", err)
	}

	type cand struct {
		rel  apiRelease
		year int
		n    int
	}
	var cands []cand
	for _, r := range raw {
		if r.Draft {
			continue
		}
		m := releaseTagRe.FindStringSubmatch(r.TagName)
		if m == nil {
			continue
		}
		year, _ := strconv.Atoi(m[1])
		n, _ := strconv.Atoi(m[2])
		cands = append(cands, cand{r, year, n})
	}
	if len(cands) == 0 {
		return nil, fmt.Errorf("no consolidated release (year.n tag) found for %s/%s", c.Owner, c.Repo)
	}
	sort.Slice(cands, func(i, j int) bool {
		if cands[i].year != cands[j].year {
			return cands[i].year > cands[j].year
		}
		return cands[i].n > cands[j].n
	})
	top := cands[0].rel

	rel := &Release{Tag: top.TagName}
	for _, a := range top.Assets {
		rel.Assets = append(rel.Assets, model.Asset{Name: a.Name, DownloadURL: a.URL, Size: a.Size})
	}
	return rel, nil
}

// getJSON performs a GET expecting a JSON body, enforcing HTTPS.
func (c *Client) getJSON(ctx context.Context, rawURL string) (io.ReadCloser, error) {
	return c.get(ctx, rawURL, "application/vnd.github+json")
}

// get performs an HTTPS-only GET and returns the response body on 2xx.
func (c *Client) get(ctx context.Context, rawURL, accept string) (io.ReadCloser, error) {
	if err := requireHTTPS(rawURL); err != nil {
		return nil, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, rawURL, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "tatsunari-installer")
	if accept != "" {
		req.Header.Set("Accept", accept)
	}
	if c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+c.Token)
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		snippet, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
		resp.Body.Close()
		return nil, fmt.Errorf("GET %s: %s: %s", rawURL, resp.Status, strings.TrimSpace(string(snippet)))
	}
	return resp.Body, nil
}

// GetBody fetches an asset URL body (HTTPS-only). Caller closes it.
func (c *Client) GetBody(ctx context.Context, rawURL string) (io.ReadCloser, error) {
	return c.get(ctx, rawURL, "")
}

// Download streams an asset URL (HTTPS-only) to dest, creating parent dirs.
func (c *Client) Download(ctx context.Context, rawURL, dest string) error {
	body, err := c.GetBody(ctx, rawURL)
	if err != nil {
		return err
	}
	defer body.Close()
	if err := os.MkdirAll(filepath.Dir(dest), 0o700); err != nil {
		return err
	}
	f, err := os.Create(dest)
	if err != nil {
		return err
	}
	if _, err := io.Copy(f, body); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

func requireHTTPS(rawURL string) error {
	u, err := url.Parse(rawURL)
	if err != nil {
		return fmt.Errorf("bad url %q: %w", rawURL, err)
	}
	if u.Scheme != "https" {
		return fmt.Errorf("refusing non-https url %q", rawURL)
	}
	return nil
}
