package install

import (
	"archive/zip"
	"os"
	"path/filepath"
	"testing"
)

// makeBundleZip writes a zip containing a "<name>.vst3/..." bundle at the root.
func makeBundleZip(t *testing.T, dir, bundleName string) string {
	t.Helper()
	zipPath := filepath.Join(dir, "plugin.zip")
	f, err := os.Create(zipPath)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	zw := zip.NewWriter(f)
	entries := map[string]string{
		bundleName + "/Contents/moduleinfo.json":      `{"name":"Test"}`,
		bundleName + "/Contents/x86_64-win/Test.vst3": "BINARY",
	}
	for name, body := range entries {
		w, err := zw.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write([]byte(body)); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return zipPath
}

func TestExtractZip(t *testing.T) {
	dir := t.TempDir()
	zipPath := makeBundleZip(t, dir, "Test Plugin.vst3")

	out := filepath.Join(dir, "stage")
	bundle, err := ExtractZip(zipPath, out)
	if err != nil {
		t.Fatalf("ExtractZip: %v", err)
	}
	if filepath.Base(bundle) != "Test Plugin.vst3" {
		t.Errorf("bundle dir = %q, want 'Test Plugin.vst3'", bundle)
	}
	got, err := os.ReadFile(filepath.Join(bundle, "Contents", "moduleinfo.json"))
	if err != nil {
		t.Fatalf("expected extracted moduleinfo.json: %v", err)
	}
	if string(got) != `{"name":"Test"}` {
		t.Errorf("moduleinfo content = %q", got)
	}
}

func TestExtractZipRejectsTraversal(t *testing.T) {
	dir := t.TempDir()
	zipPath := filepath.Join(dir, "evil.zip")
	f, _ := os.Create(zipPath)
	zw := zip.NewWriter(f)
	w, _ := zw.Create("../escape.txt")
	_, _ = w.Write([]byte("nope"))
	_ = zw.Close()
	_ = f.Close()

	if _, err := ExtractZip(zipPath, filepath.Join(dir, "stage")); err == nil {
		t.Error("expected zip-slip traversal to be rejected")
	}
}
