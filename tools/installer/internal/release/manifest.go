package release

import (
	"context"
	"encoding/json"
	"fmt"
)

// ManifestName is the release asset mapping slug -> dotted version.
const ManifestName = "manifest.json"

// FetchManifest downloads and parses manifest.json ({ "<slug>": "<version>" }).
func (c *Client) FetchManifest(ctx context.Context, rel *Release) (map[string]string, error) {
	a, ok := rel.Asset(ManifestName)
	if !ok {
		return nil, fmt.Errorf("release %s has no %s", rel.Tag, ManifestName)
	}
	body, err := c.GetBody(ctx, a.DownloadURL)
	if err != nil {
		return nil, err
	}
	defer body.Close()

	var m map[string]string
	if err := json.NewDecoder(body).Decode(&m); err != nil {
		return nil, fmt.Errorf("decode %s: %w", ManifestName, err)
	}
	return m, nil
}
