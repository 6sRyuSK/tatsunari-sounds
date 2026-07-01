package release

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"strings"
)

// ChecksumsName is the release asset with SHA-256 digests of every zip, in
// standard `sha256sum` format ("<hex>  <filename>").
const ChecksumsName = "SHA256SUMS.txt"

// Checksums maps an asset filename to its expected lowercase hex SHA-256.
type Checksums map[string]string

// FetchChecksums downloads and parses SHA256SUMS.txt.
func (c *Client) FetchChecksums(ctx context.Context, rel *Release) (Checksums, error) {
	a, ok := rel.Asset(ChecksumsName)
	if !ok {
		return nil, fmt.Errorf("release %s has no %s", rel.Tag, ChecksumsName)
	}
	body, err := c.GetBody(ctx, a.DownloadURL)
	if err != nil {
		return nil, err
	}
	defer body.Close()

	data, err := io.ReadAll(io.LimitReader(body, 1<<20))
	if err != nil {
		return nil, err
	}
	return ParseChecksums(string(data)), nil
}

// ParseChecksums parses `sha256sum` output. Each non-empty line is
// "<hex><space><space-or-star><filename>"; the filename may contain spaces, so
// we split on the first run of whitespace only.
func ParseChecksums(text string) Checksums {
	out := Checksums{}
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		hexsum := strings.ToLower(fields[0])
		// Rejoin the remainder as the filename, dropping a leading '*' binary
		// marker that sha256sum may emit.
		name := strings.TrimSpace(line[len(fields[0]):])
		name = strings.TrimPrefix(name, "*")
		name = strings.TrimSpace(name)
		if len(hexsum) != 64 || name == "" {
			continue
		}
		out[name] = hexsum
	}
	return out
}

// Verify reports whether the file at path hashes to expected (case-insensitive).
func Verify(path, expected string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return err
	}
	got := hex.EncodeToString(h.Sum(nil))
	if !strings.EqualFold(got, expected) {
		return fmt.Errorf("checksum mismatch for %s: got %s want %s", path, got, expected)
	}
	return nil
}
