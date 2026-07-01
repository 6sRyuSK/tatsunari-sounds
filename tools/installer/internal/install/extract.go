package install

import (
	"archive/zip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// bundleSuffixes are the plugin bundle directory extensions we install.
var bundleSuffixes = []string{".vst3", ".component"}

// ExtractZip unzips a per-plugin release zip into destDir and returns the path
// to the extracted plugin bundle directory (".vst3" or ".component"), which
// sits at the zip root. It preserves file modes (so Mach-O exec bits inside a
// macOS bundle survive) and rejects zip-slip path traversal.
func ExtractZip(zipPath, destDir string) (string, error) {
	zr, err := zip.OpenReader(zipPath)
	if err != nil {
		return "", err
	}
	defer zr.Close()

	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return "", err
	}
	cleanDest := filepath.Clean(destDir)

	bundleName := ""
	for _, f := range zr.File {
		name := f.Name
		// Normalize separators; zip always uses forward slashes.
		rel := filepath.FromSlash(name)
		target := filepath.Join(cleanDest, rel)
		// zip-slip guard: the resolved target must stay under destDir.
		if target != cleanDest && !strings.HasPrefix(target, cleanDest+string(os.PathSeparator)) {
			return "", fmt.Errorf("unsafe path in zip: %q", name)
		}

		if top := topBundle(name); top != "" {
			bundleName = top
		}

		if f.FileInfo().IsDir() {
			if err := os.MkdirAll(target, 0o755); err != nil {
				return "", err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return "", err
		}
		if err := writeZipFile(f, target); err != nil {
			return "", err
		}
	}
	if bundleName == "" {
		return "", fmt.Errorf("no plugin bundle (.vst3/.component) found at the root of %s", filepath.Base(zipPath))
	}
	return filepath.Join(cleanDest, bundleName), nil
}

// writeZipFile extracts a single file entry, preserving its mode.
func writeZipFile(f *zip.File, target string) error {
	rc, err := f.Open()
	if err != nil {
		return err
	}
	defer rc.Close()

	mode := f.Mode()
	if mode == 0 {
		mode = 0o644
	}
	out, err := os.OpenFile(target, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, mode.Perm())
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, rc); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

// topBundle returns the first path component of name if it is a bundle
// directory (ends in .vst3/.component), else "".
func topBundle(name string) string {
	name = filepath.ToSlash(name)
	top := name
	if i := strings.IndexByte(name, '/'); i >= 0 {
		top = name[:i]
	}
	for _, sfx := range bundleSuffixes {
		if strings.HasSuffix(top, sfx) {
			return top
		}
	}
	return ""
}
