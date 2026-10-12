package diff

import (
	"math/rand"
	"strconv"
)

// Gen produces a deterministic command stream for one seed.
type Gen struct{ r *rand.Rand }

func NewGen(seed int64) *Gen { return &Gen{r: rand.New(rand.NewSource(seed))} }

const farUnix = 4102444800 // 2100-01-01: deadlines that never fire during a run

func (g *Gen) pick(xs ...string) string { return xs[g.r.Intn(len(xs))] }

func (g *Gen) key() string {
	return g.pick("k0", "k1", "k2", "k3", "k4", "k5", "{t}a", "{t}b", "", "bin\x00\r\n")
}

func (g *Gen) value() string {
	switch g.r.Intn(8) {
	case 0:
		return strconv.Itoa(g.r.Intn(2000) - 1000)
	case 1:
		return g.pick("9223372036854775807", "-9223372036854775808", "9223372036854775806")
	case 2:
		return g.pick("1.5", "3e2", "-0.25", " 1", "007", "1.5x", "inf", "")
	case 3:
		b := make([]byte, g.r.Intn(12))
		for i := range b {
			b[i] = byte(g.r.Intn(256))
		}
		return string(b)
	default:
		b := make([]byte, g.r.Intn(20))
		for i := range b {
			b[i] = byte('a' + g.r.Intn(26))
		}
		return string(b)
	}
}

func (g *Gen) int() string {
	return g.pick(strconv.Itoa(g.r.Intn(100)-50), strconv.Itoa(g.r.Intn(5000)), "x", "-0", "9223372036854775807",
		"-9223372036854775808")
}

func (g *Gen) farSec() string { return strconv.FormatInt(farUnix+g.r.Int63n(1_000_000), 10) }
func (g *Gen) farMs() string  { return strconv.FormatInt((farUnix+g.r.Int63n(1_000_000))*1000, 10) }

// Norm says how a reply is normalised before comparison. NormNone replies are compared
// byte-for-byte on the wire.
type Norm int

const (
	NormNone Norm = iota
	NormSort      // order-free array (KEYS)
	NormTTL       // positive TTL/PTTL values depend on the clock; -2/-1 compare exactly
)

// Next returns the next command and how its reply is normalised before comparing.
func (g *Gen) Next() ([]string, Norm) {
	cmd, sorted := g.next()
	switch {
	case sorted:
		return cmd, NormSort
	case cmd[0] == "TTL" || cmd[0] == "PTTL":
		return cmd, NormTTL
	}
	return cmd, NormNone
}

func (g *Gen) next() ([]string, bool) {
	k, v := g.key(), g.value()
	switch g.r.Intn(34) {
	case 0:
		return append([]string{"SET", k, v}, g.setOptions()...), false
	case 1, 2:
		return []string{"GET", k}, false
	case 3:
		return []string{"SETNX", k, v}, false
	case 4:
		return []string{"GETSET", k, v}, false
	case 5:
		return []string{"GETDEL", k}, false
	case 6:
		return []string{"MSET", k, v, g.key(), g.value()}, false
	case 7:
		return []string{"MGET", k, g.key(), g.key()}, false
	case 8:
		return []string{"MSETNX", k, v, g.key(), g.value()}, false
	case 9:
		return []string{"APPEND", k, v}, false
	case 10:
		return []string{"STRLEN", k}, false
	case 11:
		return []string{g.pick("INCR", "DECR"), k}, false
	case 12:
		return []string{g.pick("INCRBY", "DECRBY"), k, g.int()}, false
	case 13:
		return []string{"INCRBYFLOAT", k, g.pick("0.1", "-1.5", "1e3", "abc", "3", "inf")}, false
	case 14:
		return []string{g.pick("GETRANGE", "SUBSTR"), k, g.int(), g.int()}, false
	case 15:
		return []string{"SETRANGE", k, strconv.Itoa(g.r.Intn(40) - 2), v}, false
	case 16:
		return []string{g.pick("DEL", "UNLINK"), k, g.key()}, false
	case 17:
		return []string{"EXISTS", k, g.key(), k}, false
	case 18:
		return []string{"TYPE", k}, false
	case 19:
		return []string{g.pick("RENAME", "RENAMENX"), k, g.key()}, false
	case 20:
		return append([]string{"EXPIREAT", k, g.farSec()}, g.expireFlags()...), false
	case 21:
		return append([]string{"PEXPIREAT", k, g.farMs()}, g.expireFlags()...), false
	case 22:
		return []string{g.pick("EXPIRETIME", "PEXPIRETIME"), k}, false
	case 23:
		return []string{"PERSIST", k}, false
	case 24:
		return []string{g.pick("EXPIRE", "PEXPIRE", "EXPIREAT"), k, g.pick("-1", "0")}, false
	case 25:
		return []string{"DBSIZE"}, false
	case 26:
		return []string{"KEYS", g.pick("*", "k*", "{t}?", "*b*", "[k]1")}, true
	case 27:
		return append([]string{"LCS", k, g.key()}, g.lcsOptions()...), false
	case 28:
		return append([]string{"GETEX", k}, g.getexOptions()...), false
	case 29:
		// Every connection starts in db 0 and both sides get the same SELECTs, so they stay in step.
		return []string{"SELECT", g.pick("0", "1", "2")}, false
	case 30:
		// Only error paths: a valid SETEX/PSETEX would set a relative, clock-dependent TTL.
		return [][]string{{"SETEX", k, "0", v}, {"SETEX", k, "x", v}, {"SETEX", k, "-3", v}, {"PSETEX", k, "0", v},
			{"PSETEX", k, "abc", v}, {"SETEX", k, "10"}, {"PSETEX", k}}[g.r.Intn(7)], false
	case 31:
		if g.r.Intn(4) == 0 { // rare, so keyspaces have time to fill up between flushes
			return []string{"FLUSHDB"}, false
		}
		return []string{"EXISTS", k}, false
	case 32:
		return []string{g.pick("TTL", "PTTL"), k}, false
	default:
		return g.malformed(), false
	}
}

func (g *Gen) setOptions() []string {
	var o []string
	if g.r.Intn(3) == 0 {
		o = append(o, g.pick("NX", "XX", "nx"))
	}
	if g.r.Intn(3) == 0 {
		o = append(o, "GET")
	}
	switch g.r.Intn(5) {
	case 0:
		o = append(o, "EXAT", g.farSec())
	case 1:
		o = append(o, "PXAT", g.farMs())
	case 2:
		o = append(o, "KEEPTTL")
	case 3:
		o = append(o, g.pick("EX", "PX"), g.pick("0", "-5", "x")) // invalid relative values only
	}
	return o
}

func (g *Gen) expireFlags() []string {
	if g.r.Intn(2) == 0 {
		return nil
	}
	return []string{g.pick("NX", "XX", "GT", "LT", "gt")}
}

func (g *Gen) lcsOptions() []string {
	return [][]string{nil, {"LEN"}, {"IDX"}, {"IDX", "WITHMATCHLEN"}, {"IDX", "MINMATCHLEN", "2"}, {"LEN", "IDX"}}[g.r.Intn(6)]
}

func (g *Gen) getexOptions() []string {
	return [][]string{nil, {"PERSIST"}, {"EXAT", g.farSec()}, {"PXAT", g.farMs()}, {"EX", "0"}, {"PERSIST", "EX", "1"}}[g.r.Intn(6)]
}

// Wrong arity, bad options and garbage, so error texts are compared too.
func (g *Gen) malformed() []string {
	return [][]string{{"SET", "k0"}, {"GET"}, {"SET", "k0", "v", "BOGUS"}, {"SET", "k0", "v", "NX", "XX"},
		{"MSET", "k0"}, {"EXPIRE", "k0", "10", "NX", "XX"}, {"EXPIRE", "k0", "abc"}, {"SETRANGE", "k0", "-1", "x"},
		{"INCRBY", "k0", "1.5"}, {"SELECT", "99"}, {"GETEX", "k0", "BOGUS"}, {"LCS", "k0", "k1", "BOGUS"}}[g.r.Intn(12)]
}
