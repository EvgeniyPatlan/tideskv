package diff

import (
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
	g := NewGen(seed)
	var history []string
	for step := 0; step < ops(); step++ {
		cmd, sorted := g.Next()
		history = append(history, strconv.Quote(strings.Join(cmd, " ")))
		ra, errA := a.Do(cmd...)
		rb, errB := b.Do(cmd...)
		if errA != nil || errB != nil {
			t.Fatalf("seed %d step %d %s: transport error tideskv=%v redis=%v", seed, step, cmd, errA, errB)
		}
		if sorted {
			ra, rb = SortElems(ra), SortElems(rb)
		}
		if !reflect.DeepEqual(ra, rb) {
			from := len(history) - 20
			if from < 0 {
				from = 0
			}
			t.Fatalf("seed %d step %d diverged\ncommand: %s\ntideskv: %s\nredis:   %s\nlast commands:\n  %s", seed, step,
				history[len(history)-1], ra, rb, strings.Join(history[from:], "\n  "))
		}
	}
}
