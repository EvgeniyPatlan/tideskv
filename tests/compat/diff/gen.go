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
	NormNone      Norm = iota
	NormSort           // order-free array (KEYS)
	NormTTL            // positive TTL/PTTL values depend on the clock; -2/-1 compare exactly
	NormLen            // random picks: same kind and array length (errors exact)
	NormPairs          // order-free field/value pairs (HGETALL)
	NormScanPairs      // HSCAN page: cursor exact, pairs order-free
	NormScanElems      // SSCAN page: cursor exact, elements order-free
)

// Next returns the next command and how its reply is normalised before comparing.
func (g *Gen) Next() ([]string, Norm) {
	switch g.r.Intn(4) { // a quarter of the stream goes to each family
	case 0:
		return g.collection(g.key())
	case 1:
		return g.listCmd(g.key())
	case 2:
		return g.zsetCmd(g.key())
	}
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

func (g *Gen) field() string { return g.pick("f0", "f1", "f2", "f3", "", "f\x00b", "7", "-3") }

// collection returns the next hash/set command and its normalisation. Hashes and sets stay
// small (at most 8 fields/members) so Redis keeps listpack/intset encodings, which return
// HSCAN/SSCAN in one page with cursor 0, as TidesKV does with COUNT 1000.
func (g *Gen) collection(k string) ([]string, Norm) {
	f, v := g.field(), g.value()
	switch g.r.Intn(30) {
	case 0, 1:
		return []string{"HSET", k, f, v, g.field(), g.value()}, NormNone
	case 2:
		return []string{"HSETNX", k, f, v}, NormNone
	case 3:
		return []string{"HGET", k, f}, NormNone
	case 4:
		return []string{"HMGET", k, f, g.field()}, NormNone
	case 5:
		return []string{"HDEL", k, f, g.field()}, NormNone
	case 6:
		if cmd := g.pick("HLEN", "HKEYS", "HVALS"); cmd != "HLEN" {
			return []string{cmd, k}, NormSort
		}
		return []string{"HLEN", k}, NormNone
	case 7:
		return []string{"HGETALL", k}, NormPairs
	case 8:
		return []string{"HSTRLEN", k, f}, NormNone
	case 9:
		return []string{"HEXISTS", k, f}, NormNone
	case 10:
		return []string{"HINCRBY", k, f, g.int()}, NormNone
	case 11:
		return []string{"HINCRBYFLOAT", k, f, g.pick("0.1", "-1.5", "1e3", "abc", "3", "inf")}, NormNone
	case 12:
		return append([]string{"HRANDFIELD", k}, g.randCount()...), NormLen
	case 13:
		return []string{"HSCAN", k, "0", "COUNT", "1000"}, NormScanPairs
	case 14, 15:
		return []string{"SADD", k, f, g.field(), g.field()}, NormNone
	case 16:
		return []string{"SREM", k, f, g.field()}, NormNone
	case 17:
		return []string{g.pick("SCARD", "SMEMBERS"), k}, NormSort
	case 18:
		return []string{"SISMEMBER", k, f}, NormNone
	case 19:
		return []string{"SMISMEMBER", k, f, g.field()}, NormNone
	case 20:
		return [][]string{{"SPOP", k, "1000"}, {"SPOP", k, "0"}, {"SPOP", k, "-1"}, {"SPOP", k, "x"}, {"SPOP", k, "1", "2"}}[g.r.Intn(5)], NormSort
	case 21:
		return append([]string{"SRANDMEMBER", k}, g.randCount()...), NormLen
	case 22:
		return []string{"SMOVE", k, g.key(), f}, NormNone
	case 23:
		return []string{g.pick("SINTER", "SUNION", "SDIFF"), k, g.key(), g.key()}, NormSort
	case 24:
		return []string{g.pick("SINTERSTORE", "SUNIONSTORE", "SDIFFSTORE"), g.key(), k, g.key()}, NormNone
	case 25:
		return append([]string{"SINTERCARD", g.pick("2", "1", "0", "3", "x"), k, g.key()}, g.limit()...), NormNone
	case 26:
		return []string{"SSCAN", k, "0", "COUNT", "1000"}, NormScanElems
	case 27:
		return [][]string{{"HSET", k, f}, {"HSET", k, f, v, f}, {"HMSET", k, f}, {"HSCAN", k, "x"},
			{"HSCAN", k, "0", "TYPE", "hash"}, {"HRANDFIELD", k, "1", "BOGUS"}, {"HRANDFIELD", k, "-9223372036854775808"},
			{"HRANDFIELD", k, "4611686018427387904", "WITHVALUES"}}[g.r.Intn(8)], NormNone
	default:
		return []string{"TYPE", k}, NormNone
	}
}

func (g *Gen) randCount() []string {
	return [][]string{nil, {"100"}, {"0"}, {"-5"}, {"3"}, {"100", "WITHVALUES"}, {"-4", "withvalues"}, {"x"}}[g.r.Intn(8)]
}

func (g *Gen) limit() []string {
	return [][]string{nil, {"LIMIT", "1"}, {"LIMIT", "0"}, {"LIMIT", "-1"}, {"LIMIT"}}[g.r.Intn(5)]
}

func (g *Gen) score() string {
	return g.pick("1", "2", "-1", "0", "-0", "1.5", "0.1", "1e20", "3e-7", "inf", "-inf", "+inf", "123456789012345678",
		"0x10", "x", "nan", "", " 1")
}

func (g *Gen) end() string    { return g.pick("LEFT", "RIGHT", "left", "UP") }
func (g *Gen) minmax() string { return g.pick("MIN", "MAX", "max", "LEFT") }

func (g *Gen) scoreBound() string {
	return g.pick("-inf", "+inf", "0", "1", "(1", "2", "(2", "1.5", "", "(", "x", "1e999")
}

func (g *Gen) lexBound() string { return g.pick("-", "+", "[f1", "(f1", "[f3", "(", "[", "f2", "+x") }

func (g *Gen) idx() string {
	return g.pick("0", "1", "-1", "2", "-2", "5", "-100", "100", "x", "9223372036854775807", "-9223372036854775808")
}

// listCmd: lists stay short (pushes of 1-3 elements, frequent pops), so Redis keeps listpacks.
func (g *Gen) listCmd(k string) ([]string, Norm) {
	f := g.field()
	switch g.r.Intn(22) {
	case 0, 1, 2:
		return []string{g.pick("LPUSH", "RPUSH"), k, f, g.field()}, NormNone
	case 3:
		return []string{g.pick("LPUSHX", "RPUSHX"), k, f}, NormNone
	case 4:
		return append([]string{g.pick("LPOP", "RPOP"), k}, [][]string{nil, {"2"}, {"0"}, {"-1"}, {"x"}}[g.r.Intn(5)]...), NormNone
	case 5:
		return []string{"LLEN", k}, NormNone
	case 6:
		return []string{"LINDEX", k, g.idx()}, NormNone
	case 7:
		return []string{"LSET", k, g.idx(), f}, NormNone
	case 8, 9:
		return []string{"LRANGE", k, g.idx(), g.idx()}, NormNone
	case 10:
		return []string{"LINSERT", k, g.pick("BEFORE", "after", "middle"), f, g.field()}, NormNone
	case 11:
		return []string{"LREM", k, g.pick("0", "1", "-1", "2", "x"), f}, NormNone
	case 12:
		return []string{"LTRIM", k, g.idx(), g.idx()}, NormNone
	case 13:
		return append([]string{"LPOS", k, f}, [][]string{nil, {"RANK", "-1"}, {"COUNT", "0"}, {"RANK", "2", "COUNT", "2"},
			{"MAXLEN", "1"}, {"RANK", "0"}, {"COUNT", "-1"}, {"RANK"}}[g.r.Intn(8)]...), NormNone
	case 14:
		return []string{"LMOVE", k, g.key(), g.end(), g.end()}, NormNone
	case 15:
		return []string{"RPOPLPUSH", k, g.key()}, NormNone
	case 16:
		return append([]string{"LMPOP", g.pick("1", "2", "0", "x"), k, g.key(), g.end()}, [][]string{nil, {"COUNT", "2"},
			{"COUNT", "0"}, {"COUNT"}}[g.r.Intn(4)]...), NormNone
	case 17:
		return []string{g.pick("BLPOP", "BRPOP"), k, g.key(), g.pick("0.01", "-1", "x")}, NormNone
	case 18:
		return []string{"BLMOVE", k, g.key(), g.end(), g.end(), g.pick("0.01", "x")}, NormNone
	case 19:
		return []string{"BRPOPLPUSH", k, g.key(), "0.01"}, NormNone
	case 20:
		return []string{"BLMPOP", g.pick("0.01", "x"), g.pick("1", "2", "0"), k, g.key(), g.end()}, NormNone
	default:
		return []string{"TYPE", k}, NormNone
	}
}

// lexKey names the sorted sets used by the lexicographic commands. Redis documents that BYLEX
// ranges are meaningful only when all members share one score (on mixed scores the result
// depends on its listpack/skiplist scan, which TidesKV does not mimic), so only ZADD with score 0
// ever writes these keys and no other command names them.
func (g *Gen) lexKey() string { return g.pick("lex0", "lex1") }

// zsetCmd: zsets stay small (pairs of 1-2 members), so Redis keeps listpacks (ZSCAN in score order, -0 as 0).
func (g *Gen) zsetCmd(k string) ([]string, Norm) {
	f := g.field()
	switch g.r.Intn(31) {
	case 0, 1, 2:
		return append([]string{"ZADD", k}, append(g.zaddFlags(), g.score(), f, g.score(), g.field())...), NormNone
	case 3:
		return []string{"ZINCRBY", k, g.score(), f}, NormNone
	case 4:
		return []string{"ZREM", k, f, g.field()}, NormNone
	case 5:
		if g.r.Intn(2) == 0 {
			return []string{"ZCARD", k}, NormNone
		}
		return []string{"ZSCORE", k, f}, NormNone
	case 6:
		return []string{"ZMSCORE", k, f, g.field()}, NormNone
	case 7:
		return []string{"ZCOUNT", k, g.scoreBound(), g.scoreBound()}, NormNone
	case 8:
		return []string{"ZLEXCOUNT", g.lexKey(), g.lexBound(), g.lexBound()}, NormNone
	case 9, 10:
		return append([]string{"ZRANGE", k, g.idx(), g.idx()}, g.zrangeOpts()...), NormNone
	case 11:
		return append([]string{"ZRANGE", k, g.scoreBound(), g.scoreBound(), "BYSCORE"}, g.zrangeOpts()...), NormNone
	case 12:
		return append([]string{g.pick("ZRANGEBYLEX", "ZREVRANGEBYLEX"), g.lexKey(), g.lexBound(), g.lexBound()},
			[][]string{nil, {"LIMIT", "0", "1"}, {"LIMIT", "1", "-1"}, {"LIMIT", "x", "1"}}[g.r.Intn(4)]...), NormNone
	case 13:
		return []string{g.pick("ZRANGEBYSCORE", "ZREVRANGEBYSCORE"), k, g.scoreBound(), g.scoreBound(), "WITHSCORES"}, NormNone
	case 14:
		return append([]string{g.pick("ZRANK", "ZREVRANK"), k, f}, [][]string{nil, {"WITHSCORE"}, {"x"}}[g.r.Intn(3)]...), NormNone
	case 15:
		return []string{g.pick("ZREMRANGEBYRANK", "ZREMRANGEBYSCORE", "ZREMRANGEBYLEX"), k, g.idx(), g.idx()}, NormNone
	case 16:
		return append([]string{g.pick("ZPOPMIN", "ZPOPMAX"), k}, [][]string{nil, {"1"}, {"2"}, {"0"}, {"-1"}, {"x"}}[g.r.Intn(6)]...), NormNone
	case 17:
		return append([]string{"ZMPOP", g.pick("1", "2", "0"), k, g.key(), g.minmax()}, [][]string{nil, {"COUNT", "2"}, {"COUNT", "0"}}[g.r.Intn(3)]...), NormNone
	case 18:
		return []string{g.pick("BZPOPMIN", "BZPOPMAX"), k, g.key(), g.pick("0.01", "-1")}, NormNone
	case 19:
		return []string{"BZMPOP", "0.01", g.pick("1", "2"), k, g.key(), g.minmax()}, NormNone
	case 20:
		return append([]string{"ZRANDMEMBER", k}, [][]string{nil, {"3"}, {"-5"}, {"100", "WITHSCORES"}, {"0"}, {"x"}}[g.r.Intn(6)]...), NormLen
	case 21:
		return []string{"ZSCAN", k, "0", "COUNT", g.pick("1", "1000")}, NormNone
	case 22:
		return append([]string{g.pick("ZUNION", "ZINTER", "ZDIFF"), "2", k, g.key()}, g.algebraOpts()...), NormNone
	case 23:
		return append([]string{g.pick("ZUNIONSTORE", "ZINTERSTORE", "ZDIFFSTORE"), g.key(), "2", k, g.key()}, g.storeOpts()...), NormNone
	case 24:
		return append([]string{"ZINTERCARD", g.pick("2", "1", "0", "x"), k, g.key()}, g.limit()...), NormNone
	case 25:
		return []string{"ZRANGESTORE", g.key(), k, g.idx(), g.idx()}, NormNone
	case 26:
		return []string{"ZADD", g.lexKey(), "0", f, "0", g.field()}, NormNone
	case 27:
		return []string{"ZREMRANGEBYLEX", g.lexKey(), g.lexBound(), g.lexBound()}, NormNone
	case 28:
		return append([]string{"ZRANGE", g.lexKey(), g.lexBound(), g.lexBound(), "BYLEX"},
			[][]string{nil, {"REV"}, {"LIMIT", "0", "1"}, {"WITHSCORES"}}[g.r.Intn(4)]...), NormNone
	default:
		return []string{"TYPE", k}, NormNone
	}
}

func (g *Gen) zaddFlags() []string {
	return [][]string{nil, nil, {"NX"}, {"XX"}, {"GT", "CH"}, {"LT"}, {"INCR"}, {"NX", "XX"}, {"GT", "LT"}}[g.r.Intn(9)]
}

func (g *Gen) zrangeOpts() []string {
	return [][]string{nil, {"WITHSCORES"}, {"REV"}, {"LIMIT", "0", "1"}, {"REV", "WITHSCORES"}, {"LIMIT", "1", "-1"}}[g.r.Intn(6)]
}

func (g *Gen) algebraOpts() []string {
	return [][]string{nil, {"WITHSCORES"}, {"WEIGHTS", "2", "0.5"}, {"AGGREGATE", "MAX", "WITHSCORES"},
		{"WEIGHTS", "1", "x"}, {"AGGREGATE", "avg"}}[g.r.Intn(6)]
}

func (g *Gen) storeOpts() []string {
	return [][]string{nil, {"WEIGHTS", "inf", "-inf"}, {"AGGREGATE", "MIN"}, {"WITHSCORES"}}[g.r.Intn(4)]
}
