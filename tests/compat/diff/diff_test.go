package diff

import (
	"bytes"
	"fmt"
	"os"
	"reflect"
	"strconv"
	"strings"
	"testing"

	"tideskv.local/compat/util"
)

func seeds(t *testing.T) []int64 {
	spec := os.Getenv("TIDESKV_DIFF_SEEDS")
	if spec == "" {
		spec = "1-5"
	}
	lo, hi, found := strings.Cut(spec, "-")
	a, err := strconv.ParseInt(lo, 10, 64)
	if err != nil {
		t.Fatalf("bad TIDESKV_DIFF_SEEDS %q", spec)
	}
	b := a
	if found {
		if b, err = strconv.ParseInt(hi, 10, 64); err != nil {
			t.Fatalf("bad TIDESKV_DIFF_SEEDS %q", spec)
		}
	}
	var out []int64
	for s := a; s <= b; s++ {
		out = append(out, s)
	}
	return out
}

func ops() int {
	if n, err := strconv.Atoi(os.Getenv("TIDESKV_DIFF_OPS")); err == nil && n > 0 {
		return n
	}
	return 3000
}

func mustDial(t *testing.T, addr string) *Conn {
	c, err := Dial(addr)
	if err != nil {
		t.Fatalf("dial %s: %v", addr, err)
	}
	return c
}

func TestDifferential(t *testing.T) {
	ref := os.Getenv("TIDESKV_REF_ADDR")
	if ref == "" {
		if os.Getenv("TIDESKV_DIFF_REQUIRED") != "" {
			t.Fatal("TIDESKV_DIFF_REQUIRED is set but TIDESKV_REF_ADDR is empty: no Redis 7.2 reference to diff against")
		}
		t.Skip("set TIDESKV_REF_ADDR to a Redis 7.2 server (scripts/compat.sh --diff does this)")
	}
	refConn := mustDial(t, ref)
	info, err := refConn.Do("INFO", "server")
	refConn.Close()
	if err != nil || !strings.Contains(info.Str, "redis_version:7.2.") {
		t.Fatalf("reference at %s must be Redis 7.2.x", ref)
	}
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()
	for _, seed := range seeds(t) {
		t.Run(fmt.Sprintf("seed-%d", seed), func(t *testing.T) { runSeed(t, seed, srv.HostPort(), ref) })
	}
}

func runSeed(t *testing.T, seed int64, ours, ref string) {
	a, b := mustDial(t, ours), mustDial(t, ref)
	defer a.Close()
	defer b.Close()
	for _, c := range []*Conn{a, b} {
		if _, err := c.Do("FLUSHALL"); err != nil {
			t.Fatal(err)
		}
	}
	if seed%2 == 1 { // odd seeds run the whole stream in RESP3
		for name, c := range map[string]*Conn{"tideskv": a, "redis": b} {
			r, err := c.Do("HELLO", "3") // its contents (server info) differ by design; its kind must not
			if err != nil {
				t.Fatal(err)
			}
			if r.Kind != '%' {
				t.Fatalf("seed %d: %s: HELLO 3 replied %s, want a RESP3 map", seed, name, r)
			}
		}
	}
	g := NewGen(seed)
	var history []string
	for step := 0; step < ops(); step++ {
		cmd, norm := g.Next()
		history = append(history, strconv.Quote(strings.Join(cmd, " ")))
		ra, rawA, errA := a.DoRaw(cmd...)
		rb, rawB, errB := b.DoRaw(cmd...)
		if errA != nil || errB != nil {
			t.Fatalf("seed %d step %d: transport error\ntideskv: %v\nredis:   %v\nlast commands:\n  %s", seed, step,
				errA, errB, lastCommands(history))
		}
		if !sameReply(norm, ra, rb, rawA, rawB) {
			t.Fatalf("seed %d step %d diverged\ncommand: %s\ntideskv: %s (wire %q)\nredis:   %s (wire %q)\n"+
				"last commands:\n  %s", seed, step, history[len(history)-1], ra, rawA, rb, rawB, lastCommands(history))
		}
	}
	for name, c := range map[string]*Conn{"tideskv": a, "redis": b} {
		if err := c.AssertDrained(); err != nil {
			t.Fatalf("seed %d: %s: %v\nlast commands:\n  %s", seed, name, err, lastCommands(history))
		}
	}
}

// sameReply compares un-normalised replies byte-for-byte on the wire; normalised ones
// (KEYS order, positive TTLs) are compared after normalisation of the parsed reply.
func sameReply(norm Norm, ra, rb Reply, rawA, rawB []byte) bool {
	switch norm {
	case NormSort:
		return reflect.DeepEqual(SortElems(ra), SortElems(rb))
	case NormTTL:
		return reflect.DeepEqual(PositiveAsToken(ra), PositiveAsToken(rb))
	case NormLen:
		return sameShape(ra, rb)
	case NormPairs:
		return reflect.DeepEqual(SortPairs(ra), SortPairs(rb))
	case NormScanPairs:
		return reflect.DeepEqual(NormaliseScan(ra, true), NormaliseScan(rb, true))
	case NormScanElems:
		return reflect.DeepEqual(NormaliseScan(ra, false), NormaliseScan(rb, false))
	}
	return bytes.Equal(rawA, rawB)
}

func lastCommands(history []string) string {
	from := len(history) - 20
	if from < 0 {
		from = 0
	}
	return strings.Join(history[from:], "\n  ")
}
