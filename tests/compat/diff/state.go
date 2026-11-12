package diff

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
)

const maxDiffLines = 10

// DumpState returns a canonical, server-independent view of the whole dataset: key ->
// "type|ttl|" + %q of the canonical elements joined by NUL. TTL is "-1" or "+" (positive).
// SCAN cursors are opaque; the result does not depend on SCAN order.
func DumpState(c *Conn) (map[string]string, error) {
	keys, err := scanAll(c)
	if err != nil {
		return nil, err
	}
	out := make(map[string]string, len(keys))
	for _, k := range keys {
		v, ok, err := dumpKey(c, k)
		if err != nil {
			return nil, err
		}
		if ok {
			out[k] = v
		}
	}
	return out, nil
}

func scanAll(c *Conn) ([]string, error) {
	var keys []string
	cursor := "0"
	for {
		r, err := c.Do("SCAN", cursor, "COUNT", "1000")
		if err != nil {
			return nil, fmt.Errorf("SCAN %s: %w", cursor, err)
		}
		if r.Kind != '*' || len(r.Elems) != 2 || r.Elems[1].Kind != '*' {
			return nil, fmt.Errorf("SCAN %s: unexpected reply %s", cursor, r)
		}
		for _, e := range r.Elems[1].Elems {
			keys = append(keys, e.Str)
		}
		cursor = r.Elems[0].Str
		if cursor == "0" {
			return keys, nil
		}
	}
}

// do runs a command and turns error replies into Go errors.
func do(c *Conn, args ...string) (Reply, error) {
	r, err := c.Do(args...)
	if err != nil {
		return r, fmt.Errorf("%s: %w", args[0], err)
	}
	if r.Kind == '-' {
		return r, fmt.Errorf("%s %q: error reply %q", args[0], args[1:], r.Str)
	}
	return r, nil
}

// dumpKey returns ok=false when the key disappeared (expired) between SCAN and now.
func dumpKey(c *Conn, key string) (string, bool, error) {
	tr, err := do(c, "TYPE", key)
	if err != nil {
		return "", false, err
	}
	typ := tr.Str
	if typ == "none" {
		return "", false, nil
	}
	elems, err := canonicalElems(c, typ, key)
	if err != nil {
		return "", false, err
	}
	ttl, err := do(c, "TTL", key)
	if err != nil {
		return "", false, err
	}
	n, err := strconv.ParseInt(ttl.Str, 10, 64)
	if err != nil {
		return "", false, fmt.Errorf("TTL %q: bad reply %s", key, ttl)
	}
	t, skip, err := ttlClass(n)
	if err != nil {
		return "", false, fmt.Errorf("TTL %q: %w", key, err)
	}
	if skip {
		return "", false, nil
	}
	return encodeValue(typ, t, elems), true, nil
}

// ttlClass maps a TTL reply to "-1" (no expiry) or "+" (expiring; includes 0, which servers
// report for a positive sub-second remainder). skip=true means the key is gone (-2).
func ttlClass(n int64) (class string, skip bool, err error) {
	switch {
	case n == -1:
		return "-1", false, nil
	case n == -2:
		return "", true, nil
	case n >= 0:
		return "+", false, nil
	}
	return "", false, fmt.Errorf("unexpected negative TTL %d", n)
}

// encodeValue renders "type|ttl|" followed by each element %q-quoted and space-joined, so
// ["a\x00b"] and ["a","b"] stay distinct.
func encodeValue(typ, ttl string, elems []string) string {
	q := make([]string, len(elems))
	for i, e := range elems {
		q[i] = strconv.Quote(e)
	}
	return typ + "|" + ttl + "|" + strings.Join(q, " ")
}

func canonicalElems(c *Conn, typ, key string) ([]string, error) {
	switch typ {
	case "string":
		r, err := do(c, "GET", key)
		return []string{r.Str}, err
	case "hash":
		r, err := do(c, "HGETALL", key)
		if err != nil {
			return nil, err
		}
		return sortedPairs(r)
	case "set":
		r, err := do(c, "SMEMBERS", key)
		if err != nil {
			return nil, err
		}
		s := flat(r)
		sort.Strings(s)
		return s, nil
	case "zset":
		r, err := do(c, "ZRANGE", key, "0", "-1", "WITHSCORES")
		return flat(r), err
	case "list":
		r, err := do(c, "LRANGE", key, "0", "-1")
		return flat(r), err
	}
	return nil, fmt.Errorf("key %q: unsupported type %q", key, typ)
}

func flat(r Reply) []string {
	s := make([]string, len(r.Elems))
	for i, e := range r.Elems {
		s[i] = e.Str
	}
	return s
}

func sortedPairs(r Reply) ([]string, error) {
	f := flat(r)
	if len(f)%2 != 0 {
		return nil, fmt.Errorf("HGETALL: odd element count %d", len(f))
	}
	pairs := make([][2]string, len(f)/2)
	for i := range pairs {
		pairs[i] = [2]string{f[2*i], f[2*i+1]}
	}
	sort.Slice(pairs, func(i, j int) bool { return pairs[i][0] < pairs[j][0] })
	out := make([]string, 0, len(f))
	for _, p := range pairs {
		out = append(out, p[0], p[1])
	}
	return out, nil
}

// DiffState lists up to 10 differing keys (sorted) as "key: tideskv=… kvrocks=…".
func DiffState(a, b map[string]string) []string {
	seen := map[string]bool{}
	var keys []string
	for _, m := range []map[string]string{a, b} {
		for k := range m {
			if !seen[k] {
				seen[k] = true
				keys = append(keys, k)
			}
		}
	}
	sort.Strings(keys)
	var out []string
	for _, k := range keys {
		av, aok := a[k]
		bv, bok := b[k]
		if aok && bok && av == bv {
			continue
		}
		out = append(out, fmt.Sprintf("%s: tideskv=%s kvrocks=%s", k, show(av, aok), show(bv, bok)))
		if len(out) == maxDiffLines {
			break
		}
	}
	return out
}

func show(v string, ok bool) string {
	if !ok {
		return "<missing>"
	}
	return v
}
