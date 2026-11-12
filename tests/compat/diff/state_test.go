package diff

import (
	"reflect"
	"testing"

	"tideskv.local/compat/util"
)

func TestDumpStateCanonical(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()
	c, err := Dial(srv.HostPort())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	for _, cmd := range [][]string{
		{"SET", "s", "v"}, {"HSET", "h", "b", "2", "a", "1"}, {"SADD", "st", "y", "x"},
		{"ZADD", "z", "2", "b", "1", "a"}, {"RPUSH", "l", "1", "2"}, {"EXPIRE", "s", "100"},
	} {
		if r, err := c.Do(cmd...); err != nil || r.Kind == '-' {
			t.Fatalf("%v: %v %v", cmd, r, err)
		}
	}
	got, err := DumpState(c)
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]string{
		"s":  "string|+|\"v\"",
		"h":  "hash|-1|\"a\" \"1\" \"b\" \"2\"",
		"st": "set|-1|\"x\" \"y\"",
		"z":  "zset|-1|\"a\" \"1\" \"b\" \"2\"",
		"l":  "list|-1|\"1\" \"2\"",
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v\nwant %v", got, want)
	}
}

func TestDiffStateReportsBothSides(t *testing.T) {
	a := map[string]string{"k1": "x", "k2": "same", "only": "1"}
	b := map[string]string{"k1": "y", "k2": "same"}
	got := DiffState(a, b)
	if len(got) != 2 {
		t.Fatalf("want 2 lines, got %v", got)
	}
	if got[0] != `k1: tideskv=x kvrocks=y` {
		t.Fatalf("line0 %q", got[0])
	}
}

func TestEncodeValueDistinguishesNulFromSplit(t *testing.T) {
	a := encodeValue("list", "-1", []string{"a\x00b"})
	b := encodeValue("list", "-1", []string{"a", "b"})
	if a == b {
		t.Fatalf("encodings collide: %s", a)
	}
}

func TestTTLClass(t *testing.T) {
	for n, want := range map[int64]string{-1: "-1", 0: "+", 1: "+", 100: "+"} {
		got, skip, err := ttlClass(n)
		if err != nil || skip || got != want {
			t.Fatalf("ttlClass(%d) = %q %v %v, want %q", n, got, skip, err, want)
		}
	}
	if _, skip, err := ttlClass(-2); err != nil || !skip {
		t.Fatalf("-2 should skip")
	}
	if _, _, err := ttlClass(-5); err == nil {
		t.Fatalf("-5 should error")
	}
}

func TestDumpStateSubSecondTTLIsExpiring(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()
	c, err := Dial(srv.HostPort())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	for _, cmd := range [][]string{{"SET", "k", "v"}, {"PEXPIRE", "k", "400"}} {
		if r, err := c.Do(cmd...); err != nil || r.Kind == '-' {
			t.Fatalf("%v: %v %v", cmd, r, err)
		}
	}
	got, err := DumpState(c)
	if err != nil {
		t.Fatal(err)
	}
	if want := "string|+|\"v\""; got["k"] != want {
		t.Fatalf("got %q, want %q", got["k"], want)
	}
}
