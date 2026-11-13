package diff

import "strconv"

// Case is one Kvrocks common-case workload: Setup seeds a small dataset and Next
// draws one command from the case's mix.
type Case struct {
	Name  string
	Setup func(g *Gen) [][]string
	Next  func(g *Gen) ([]string, Norm)
}

// Cases returns C1..C8 in order.
func Cases() []Case {
	return []Case{
		{"C1", c1Setup, c1Next},
		{"C2", c2Setup, c2Next},
		{"C3", c3Setup, c3Next},
		{"C4", c4Setup, c4Next},
		{"C5", c5Setup, c5Next},
		{"C6", c6Setup, c6Next},
		{"C7", c7Setup, c7Next},
		{"C8", c8Setup, c8Next},
	}
}

// num returns an index in 1..n, or (10% of the time) one just beyond it.
func (g *Gen) num(n int) int {
	if g.r.Intn(10) == 0 {
		return n + 1 + g.r.Intn(n/10+1)
	}
	return 1 + g.r.Intn(n)
}

func (g *Gen) name(prefix string, n int) string { return prefix + strconv.Itoa(g.num(n)) }

// distinct draws count different names via g.name, rejecting repeats.
func (g *Gen) distinct(prefix string, n, count int) []string {
	seen := map[string]bool{}
	out := make([]string, 0, count)
	for len(out) < count {
		k := g.name(prefix, n)
		if !seen[k] {
			seen[k] = true
			out = append(out, k)
		}
	}
	return out
}

func (g *Gen) fixed(n int) string {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte('a' + g.r.Intn(26))
	}
	return string(b)
}

func idxKey(prefix string, i int) string { return prefix + strconv.Itoa(i) }

// C1: plain string cache with TTLs.
func c1Setup(g *Gen) [][]string {
	var out [][]string
	for i := 1; i <= 200; i++ {
		cmd := []string{"SET", idxKey("c:", i), g.fixed(64)}
		if i > 160 {
			cmd = append(cmd, "EX", "1")
		}
		out = append(out, cmd)
	}
	return out
}

func c1Next(g *Gen) ([]string, Norm) {
	k := g.name("c:", 200)
	switch g.r.Intn(10) {
	case 0, 1, 2, 3, 4, 5:
		return []string{"GET", k}, NormNone
	case 6, 7, 8:
		return []string{"SET", k, g.value(), "EX", g.pick("60", "3600")}, NormNone
	}
	return []string{"TTL", k}, NormTTL
}

// C2: session hashes.
func c2Setup(g *Gen) [][]string {
	var out [][]string
	for i := 1; i <= 50; i++ {
		cmd := []string{"HSET", idxKey("sess:", i)}
		for f := 0; f < 30; f++ {
			cmd = append(cmd, "f"+strconv.Itoa(f), g.fixed(16))
		}
		out = append(out, cmd)
	}
	return out
}

func c2Next(g *Gen) ([]string, Norm) {
	k := g.name("sess:", 50)
	switch g.r.Intn(10) {
	case 0, 1, 2, 3:
		return []string{"HGETALL", k}, NormPairs
	case 4, 5, 6:
		cmd := []string{"HSET", k}
		for _, i := range g.r.Perm(30)[:1+g.r.Intn(3)] {
			cmd = append(cmd, "f"+strconv.Itoa(i), g.value())
		}
		return cmd, NormNone
	case 7:
		return []string{"EXPIRE", k, "1800"}, NormNone
	}
	return []string{"TTL", k}, NormTTL
}

// C3: counters, 80% of keys from the first 10.
func c3Setup(g *Gen) [][]string {
	var out [][]string
	for i := 1; i <= 100; i++ {
		out = append(out, []string{"SET", idxKey("ctr:", i), "0"})
	}
	return out
}

func (g *Gen) ctrKey() string {
	if g.r.Intn(10) < 8 {
		return idxKey("ctr:", 1+g.r.Intn(10))
	}
	return g.name("ctr:", 100)
}

func c3Next(g *Gen) ([]string, Norm) {
	switch g.r.Intn(3) {
	case 0:
		return []string{"INCRBY", g.ctrKey(), strconv.Itoa(1 + g.r.Intn(10))}, NormNone
	case 1:
		return []string{"HINCRBY", idxKey("hstats:", 1+g.r.Intn(10)), "f" + strconv.Itoa(g.r.Intn(5)),
			strconv.Itoa(1 + g.r.Intn(10))}, NormNone
	}
	return []string{"GET", g.ctrKey()}, NormNone
}

// C4: sets.
func c4Setup(g *Gen) [][]string {
	var out [][]string
	for s := 1; s <= 3; s++ {
		cmd := []string{"SADD", idxKey("set:", s)}
		for m := 1; m <= 200; m++ {
			cmd = append(cmd, idxKey("m:", m))
		}
		out = append(out, cmd)
	}
	return out
}

func c4Next(g *Gen) ([]string, Norm) {
	k := g.name("set:", 3)
	switch g.r.Intn(10) {
	case 0, 1, 2:
		return []string{"SADD", k, g.name("m:", 200)}, NormNone
	case 3, 4, 5, 6, 7:
		return []string{"SISMEMBER", k, g.name("m:", 200)}, NormNone
	}
	return []string{"SSCAN", k, "0", "COUNT", "1000"}, NormScanElems
}

// C5: leaderboards.
func c5Setup(g *Gen) [][]string {
	var out [][]string
	for b := 1; b <= 2; b++ {
		cmd := []string{"ZADD", idxKey("board:", b)}
		for m := 1; m <= 300; m++ {
			cmd = append(cmd, strconv.Itoa(g.r.Intn(10000)), idxKey("p:", m))
		}
		out = append(out, cmd)
	}
	return out
}

func c5Next(g *Gen) ([]string, Norm) {
	k := g.name("board:", 2)
	switch g.r.Intn(4) {
	case 0:
		return []string{"ZINCRBY", k, strconv.Itoa(1 + g.r.Intn(100)), g.name("p:", 300)}, NormNone
	case 1:
		return []string{"ZREVRANGE", k, "0", "9", "WITHSCORES"}, NormNone
	case 2:
		return []string{"ZREVRANK", k, g.name("p:", 300)}, NormNone
	}
	return []string{"ZSCORE", k, g.name("p:", 300)}, NormNone
}

// C6: list feeds.
func c6Setup(g *Gen) [][]string {
	var out [][]string
	for i := 1; i <= 20; i++ {
		cmd := []string{"RPUSH", idxKey("feed:", i)}
		for n := 1 + g.r.Intn(50); n > 0; n-- {
			cmd = append(cmd, g.fixed(16))
		}
		out = append(out, cmd)
	}
	return out
}

func c6Next(g *Gen) ([]string, Norm) {
	k := g.name("feed:", 20)
	switch g.r.Intn(3) {
	case 0:
		return []string{"LPUSH", k, g.value()}, NormNone
	case 1:
		return []string{"LTRIM", k, "0", "999"}, NormNone
	}
	return []string{"LRANGE", k, "0", "49"}, NormNone
}

// C7: multi-key string access.
func c7Setup(g *Gen) [][]string {
	var out [][]string
	for i := 1; i <= 500; i++ {
		out = append(out, []string{"SET", idxKey("mk:", i), g.fixed(32)})
	}
	return out
}

func c7Next(g *Gen) ([]string, Norm) {
	if g.r.Intn(2) == 0 {
		return append([]string{"MGET"}, g.distinct("mk:", 500, 20)...), NormNone
	}
	cmd := []string{"MSET"}
	for _, k := range g.distinct("mk:", 500, 10) {
		cmd = append(cmd, k, g.value())
	}
	return cmd, NormNone
}

// C8: mixed workload (C1 + C2 + C5) with an occasional SCAN.
func c8Setup(g *Gen) [][]string {
	out := c1Setup(g)
	out = append(out, c2Setup(g)...)
	return append(out, c5Setup(g)...)
}

func c8Next(g *Gen) ([]string, Norm) {
	if g.r.Intn(100) < 2 {
		return []string{"SCAN", "0", "MATCH", "c:1*", "COUNT", "1000"}, NormScanElems
	}
	switch g.r.Intn(3) {
	case 0:
		return c1Next(g)
	case 1:
		return c2Next(g)
	}
	return c5Next(g)
}
