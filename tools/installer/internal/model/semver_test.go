package model

import "testing"

func TestParseSemVer(t *testing.T) {
	cases := map[string]SemVer{
		"0.2.1":     {0, 2, 1},
		"1.0.0":     {1, 0, 0},
		"v0.4.0":    {0, 4, 0},
		" 10.20.3 ": {10, 20, 3},
	}
	for in, want := range cases {
		got, err := ParseSemVer(in)
		if err != nil {
			t.Fatalf("ParseSemVer(%q) error: %v", in, err)
		}
		if got != want {
			t.Errorf("ParseSemVer(%q) = %+v, want %+v", in, got, want)
		}
	}
	for _, bad := range []string{"", "1", "1.2", "1.2.3.4", "a.b.c", "1.-2.3"} {
		if _, err := ParseSemVer(bad); err == nil {
			t.Errorf("ParseSemVer(%q) expected error", bad)
		}
	}
}

func TestCompareSemVer(t *testing.T) {
	lt := func(a, b string) {
		t.Helper()
		av, _ := ParseSemVer(a)
		bv, _ := ParseSemVer(b)
		if CompareSemVer(av, bv) != -1 {
			t.Errorf("expected %s < %s", a, b)
		}
		if CompareSemVer(bv, av) != 1 {
			t.Errorf("expected %s > %s", b, a)
		}
	}
	lt("0.1.0", "0.2.0")
	lt("0.1.9", "0.2.0")
	lt("0.1.1", "0.1.2")
	lt("0.9.9", "1.0.0")
	eqv, _ := ParseSemVer("1.2.3")
	if CompareSemVer(eqv, eqv) != 0 {
		t.Error("equal versions should compare 0")
	}
}

func TestStateFor(t *testing.T) {
	cases := []struct {
		installed, latest string
		want              InstallState
	}{
		{"", "0.2.1", StateNotInstalled},
		{"0.2.0", "0.2.1", StateUpdateAvailable},
		{"0.2.1", "0.2.1", StateUpToDate},
		{"0.3.0", "0.2.1", StateUpToDate}, // newer-than-latest still counts as up to date
		{"garbage", "0.2.1", StateInstalledUnknown},
	}
	for _, c := range cases {
		if got := StateFor(c.installed, c.latest); got != c.want {
			t.Errorf("StateFor(%q,%q) = %v, want %v", c.installed, c.latest, got, c.want)
		}
	}
}
