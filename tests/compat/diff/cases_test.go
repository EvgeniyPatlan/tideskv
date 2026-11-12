package diff

import (
	"reflect"
	"strconv"
	"strings"
	"testing"
)

var allowedCmds = map[string][]string{
	"C1": {"GET", "SET", "TTL"},
	"C2": {"HGETALL", "HSET", "EXPIRE", "TTL"},
	"C3": {"INCRBY", "HINCRBY", "GET"},
	"C4": {"SADD", "SISMEMBER", "SSCAN"},
	"C5": {"ZINCRBY", "ZREVRANGE", "ZREVRANK", "ZSCORE"},
	"C6": {"LPUSH", "LTRIM", "LRANGE"},
	"C7": {"MGET", "MSET"},
	"C8": {"GET", "SET", "TTL", "HGETALL", "HSET", "EXPIRE", "ZINCRBY", "ZREVRANGE", "ZREVRANK", "ZSCORE", "SCAN"},
}

func TestCasesNamesInOrder(t *testing.T) {
	var got []string
	for _, c := range Cases() {
		got = append(got, c.Name)
	}
	want := []string{"C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("names %v, want %v", got, want)
	}
}

func TestCaseNextUsesOnlyItsCommands(t *testing.T) {
	for _, c := range Cases() {
		allowed := map[string]bool{}
		for _, n := range allowedCmds[c.Name] {
			allowed[n] = true
		}
		for seed := int64(1); seed <= 3; seed++ {
			g := NewGen(seed)
			for i := 0; i < 2000; i++ {
				cmd, _ := c.Next(g)
				if len(cmd) == 0 || !allowed[strings.ToUpper(cmd[0])] {
					t.Fatalf("%s seed %d: unexpected command %q", c.Name, seed, cmd)
				}
				checkShortExpire(t, c.Name, cmd)
			}
		}
	}
}

func checkShortExpire(t *testing.T, name string, cmd []string) {
	t.Helper()
	switch {
	case cmd[0] == "EXPIRE" && (len(cmd) != 3 || cmd[2] != "1800"):
		t.Fatalf("%s: bad EXPIRE %q", name, cmd)
	case cmd[0] == "SET" && (len(cmd) != 5 || cmd[3] != "EX" || (cmd[4] != "60" && cmd[4] != "3600")):
		t.Fatalf("%s: bad SET %q", name, cmd)
	}
}

func TestCaseDeterministic(t *testing.T) {
	for _, c := range Cases() {
		g1, g2 := NewGen(7), NewGen(7)
		if !reflect.DeepEqual(c.Setup(g1), c.Setup(g2)) {
			t.Fatalf("%s: setup differs", c.Name)
		}
		for i := 0; i < 500; i++ {
			a, na := c.Next(g1)
			b, nb := c.Next(g2)
			if !reflect.DeepEqual(a, b) || na != nb {
				t.Fatalf("%s: stream differs at %d: %q vs %q", c.Name, i, a, b)
			}
		}
	}
}

func TestC1SetupExpiresLastFifth(t *testing.T) {
	c := Cases()[0]
	n := 0
	for _, cmd := range c.Setup(NewGen(7)) {
		if len(cmd) >= 2 && cmd[len(cmd)-2] == "EX" && cmd[len(cmd)-1] == "1" {
			k, err := strconv.Atoi(strings.TrimPrefix(cmd[1], "c:"))
			if err != nil || k < 161 || k > 200 {
				t.Fatalf("EX 1 on %q", cmd[1])
			}
			n++
		}
	}
	if n != 40 {
		t.Fatalf("%d EX 1 commands, want 40", n)
	}
}

func TestC3Skew(t *testing.T) {
	c := Cases()[2]
	g := NewGen(11)
	hot, total := 0, 0
	for i := 0; i < 5000; i++ {
		cmd, _ := c.Next(g)
		total++
		k := cmd[1]
		if strings.HasPrefix(k, "hstats:") {
			hot++
			continue
		}
		if n, err := strconv.Atoi(strings.TrimPrefix(k, "ctr:")); err == nil && n >= 1 && n <= 10 {
			hot++
		}
	}
	if hot*100 < total*70 {
		t.Fatalf("hot %d of %d < 70%%", hot, total)
	}
}
