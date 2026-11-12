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
		"h":  "hash|-1|\"a\\x001\\x00b\\x002\"",
		"st": "set|-1|\"x\\x00y\"",
		"z":  "zset|-1|\"a\\x001\\x00b\\x002\"",
		"l":  "list|-1|\"1\\x002\"",
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
