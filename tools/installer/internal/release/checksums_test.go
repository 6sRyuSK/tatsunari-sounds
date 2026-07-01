package release

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"
)

func TestParseChecksums(t *testing.T) {
	// sha256sum-style output; note the two-space separator and a filename with
	// a space to prove we split on the first field only.
	text := `0000000000000000000000000000000000000000000000000000000000000001  a.zip
0000000000000000000000000000000000000000000000000000000000000002 *b.zip
00000000000000000000000000000000000000000000000000000000000000AB  name with space.zip

garbage-line
short  x.zip
`
	got := ParseChecksums(text)
	if got["a.zip"] != "0000000000000000000000000000000000000000000000000000000000000001" {
		t.Errorf("a.zip = %q", got["a.zip"])
	}
	if got["b.zip"] != "0000000000000000000000000000000000000000000000000000000000000002" {
		t.Errorf("b.zip (star marker) = %q", got["b.zip"])
	}
	if got["name with space.zip"] != "00000000000000000000000000000000000000000000000000000000000000ab" {
		t.Errorf("spaced name / lowercasing failed: %q", got["name with space.zip"])
	}
	if _, ok := got["x.zip"]; ok {
		t.Error("short hash line should be rejected")
	}
}

func TestVerify(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "f.bin")
	data := []byte("hello tatsunari")
	if err := os.WriteFile(p, data, 0o644); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(data)
	good := hex.EncodeToString(sum[:])

	if err := Verify(p, good); err != nil {
		t.Errorf("Verify with correct sum failed: %v", err)
	}
	if err := Verify(p, "deadbeef"); err == nil {
		t.Error("Verify with wrong sum should fail")
	}
}
