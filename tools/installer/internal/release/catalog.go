package release

import (
	"context"
	"encoding/json"
	"fmt"
)

// CatalogName is the optional release asset with human-facing plugin metadata,
// generated from plugins/*/plugin.toml. Absent on older releases (e.g. 2026.2),
// in which case the installer falls back to title-cased slugs.
const CatalogName = "catalog.json"

// CatalogEntry mirrors one object in catalog.json.
type CatalogEntry struct {
	Name      string   `json:"name"`
	Slug      string   `json:"slug"`
	Category  string   `json:"category"`
	Formats   []string `json:"formats"`
	Status    string   `json:"status"`
	Version   string   `json:"version"`
	Reference string   `json:"reference"`
}

// FetchCatalog downloads catalog.json if present and returns slug -> entry. A
// missing catalog is not an error: it returns (nil, nil) so callers degrade to
// title-cased slugs.
func (c *Client) FetchCatalog(ctx context.Context, rel *Release) (map[string]CatalogEntry, error) {
	a, ok := rel.Asset(CatalogName)
	if !ok {
		return nil, nil
	}
	body, err := c.GetBody(ctx, a.DownloadURL)
	if err != nil {
		return nil, err
	}
	defer body.Close()

	var entries []CatalogEntry
	if err := json.NewDecoder(body).Decode(&entries); err != nil {
		return nil, fmt.Errorf("decode %s: %w", CatalogName, err)
	}
	out := make(map[string]CatalogEntry, len(entries))
	for _, e := range entries {
		out[e.Slug] = e
	}
	return out, nil
}
