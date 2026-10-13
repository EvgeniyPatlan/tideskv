package diff

import (
	"bufio"
	"io"
	"net"
	"reflect"
	"testing"
)

// pipeConn returns a Conn whose peer drains the request and answers with reply verbatim.
func pipeConn(t *testing.T, reply string) *Conn {
	t.Helper()
	client, server := net.Pipe()
	t.Cleanup(func() { _ = client.Close(); _ = server.Close() })
	go func() {
		go func() { _, _ = io.Copy(io.Discard, server) }()
		_, _ = server.Write([]byte(reply))
	}()
	return &Conn{c: client, rd: reader{r: bufio.NewReader(client)}}
}

func TestReaderRejectsBadFraming(t *testing.T) {
	for name, reply := range map[string]string{
		"non-numeric bulk length":  "$abc\r\n",
		"bulk length below -1":     "$-2\r\n",
		"non-numeric array length": "*x\r\n",
		"array length below -1":    "*-5\r\n",
		"bad bulk trailer":         "$3\r\nfooXY",
		"bare LF line":             "+OK\n",
		"empty line":               "\r\n",
		"unknown type":             "?1\r\n",
		"RESP3 set null length":    "~-1\r\n",
		"RESP3 map null length":    "%-1\r\n",
		"bare LF inside array":     "*1\r\n:1\n",
	} {
		t.Run(name, func(t *testing.T) {
			if r, err := pipeConn(t, reply).Do("PING"); err == nil {
				t.Fatalf("reply %q parsed as %s, want an error", reply, r)
			}
		})
	}
}

func TestReaderParsesAndRecordsRaw(t *testing.T) {
	const wire = "*3\r\n$3\r\nfoo\r\n$-1\r\n:7\r\n"
	r, raw, err := pipeConn(t, wire).DoRaw("PING")
	if err != nil {
		t.Fatal(err)
	}
	if got := r.String(); got != `[$"foo", $<nil>, :"7"]` {
		t.Fatalf("parsed %s", got)
	}
	if string(raw) != wire {
		t.Fatalf("raw %q, want %q", raw, wire)
	}
}

func TestAssertDrainedDetectsTrailingReply(t *testing.T) {
	c := pipeConn(t, "+OK\r\n+EXTRA\r\n")
	if _, err := c.Do("PING"); err != nil {
		t.Fatal(err)
	}
	if err := c.AssertDrained(); err == nil {
		t.Fatal("trailing +EXTRA reply went unnoticed")
	}
	clean := pipeConn(t, "+OK\r\n")
	if _, err := clean.Do("PING"); err != nil {
		t.Fatal(err)
	}
	if err := clean.AssertDrained(); err != nil {
		t.Fatalf("clean connection reported %v", err)
	}
}

func TestShapeNormalisers(t *testing.T) {
	a := Reply{Kind: '*', Elems: []Reply{{Kind: '$', Str: "x"}, {Kind: '$', Str: "y"}}}
	b := Reply{Kind: '*', Elems: []Reply{{Kind: '$', Str: "z"}, {Kind: '$', Str: "z"}}}
	if !sameShape(a, b) {
		t.Fatal("arrays of equal length must compare equal under NormLen")
	}
	if sameShape(a, Reply{Kind: '*'}) {
		t.Fatal("different lengths must differ")
	}
	if sameShape(Reply{Kind: '-', Str: "ERR a"}, Reply{Kind: '-', Str: "ERR b"}) {
		t.Fatal("errors must still compare exactly")
	}
	p := Reply{Kind: '*', Elems: []Reply{{Kind: '$', Str: "b"}, {Kind: '$', Str: "2"}, {Kind: '$', Str: "a"}, {Kind: '$', Str: "1"}}}
	q := Reply{Kind: '*', Elems: []Reply{{Kind: '$', Str: "a"}, {Kind: '$', Str: "1"}, {Kind: '$', Str: "b"}, {Kind: '$', Str: "2"}}}
	if !reflect.DeepEqual(SortPairs(p), SortPairs(q)) {
		t.Fatal("pair order must not matter")
	}
	s1 := Reply{Kind: '*', Elems: []Reply{{Kind: '$', Str: "0"}, p}}
	s2 := Reply{Kind: '*', Elems: []Reply{{Kind: '$', Str: "0"}, q}}
	if !reflect.DeepEqual(NormaliseScan(s1, true), NormaliseScan(s2, true)) {
		t.Fatal("HSCAN pages with the same pairs must compare equal")
	}
}

func TestReaderParsesResp3(t *testing.T) {
	for wire, want := range map[string]string{
		",1.5\r\n":                     `,"1.5"`,
		"_\r\n":                        `_""`,
		"#t\r\n":                       `#"t"`,
		"%1\r\n+a\r\n:1\r\n":           `[+"a", :"1"]`,
		"~2\r\n$1\r\na\r\n$1\r\nb\r\n": `[$"a", $"b"]`,
		"=7\r\ntxt:abc\r\n":            `="txt:abc"`,
	} {
		r, raw, err := pipeConn(t, wire).DoRaw("PING")
		if err != nil {
			t.Fatalf("%q: %v", wire, err)
		}
		if r.String() != want || string(raw) != wire {
			t.Fatalf("%q parsed as %s (raw %q), want %s", wire, r, raw, want)
		}
	}
}

func TestNormalisersAcceptResp3Kinds(t *testing.T) {
	mk := func(kind byte, xs ...string) Reply {
		r := Reply{Kind: kind}
		for _, x := range xs {
			r.Elems = append(r.Elems, Reply{Kind: '$', Str: x})
		}
		return r
	}
	if !reflect.DeepEqual(SortPairs(mk('%', "b", "2", "a", "1")), mk('%', "a", "1", "b", "2")) {
		t.Fatal("HGETALL map pairs must sort and keep kind %")
	}
	if !reflect.DeepEqual(SortElems(mk('~', "b", "a")), mk('~', "a", "b")) {
		t.Fatal("SMEMBERS set must sort and keep kind ~")
	}
	s := Reply{Kind: '*', Elems: []Reply{{Kind: '$', Str: "0"}, mk('~', "b", "a")}}
	if got := NormaliseScan(s, false).Elems[1]; !reflect.DeepEqual(got, mk('~', "a", "b")) {
		t.Fatalf("scan page not sorted: %s", got)
	}
	if !sameShape(mk('~', "a"), mk('~', "z")) || sameShape(mk('~', "a"), mk('*', "a")) {
		t.Fatal("sameShape must compare RESP3 kinds and lengths")
	}
}
