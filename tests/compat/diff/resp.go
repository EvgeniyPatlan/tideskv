// Package diff runs the same random command stream against tideskv and Redis 7.2 and
// compares every reply exactly (spec §10.3 differential tests).
package diff

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Reply is a parsed RESP2/RESP3 reply. Kind is the RESP type byte (+ - : $ * and, in RESP3,
// , # ( _ = ~ > %); maps keep Kind '%' with their pairs flattened into Elems.
type Reply struct {
	Kind  byte
	Str   string
	Null  bool
	Elems []Reply
}

func (r Reply) String() string {
	switch {
	case r.Null:
		return string(r.Kind) + "<nil>"
	case isAggregate(r.Kind):
		parts := make([]string, len(r.Elems))
		for i, e := range r.Elems {
			parts[i] = e.String()
		}
		return "[" + strings.Join(parts, ", ") + "]"
	default:
		return string(r.Kind) + strconv.Quote(r.Str)
	}
}

// reader parses strict RESP2 and records every byte it consumed, so callers can compare the
// raw wire bytes as well as the parsed reply.
type reader struct {
	r   *bufio.Reader
	raw []byte
}

func (rd *reader) line() (string, error) {
	s, err := rd.r.ReadString('\n')
	rd.raw = append(rd.raw, s...)
	if err != nil {
		return "", fmt.Errorf("reading reply line %q: %w", s, err)
	}
	if !strings.HasSuffix(s, "\r\n") {
		return "", fmt.Errorf("reply line %q is not CRLF-terminated", s)
	}
	return strings.TrimSuffix(s, "\r\n"), nil
}

// length parses a $/* header length; only -1 (null) and non-negative values are valid.
func length(kind byte, s string) (int, error) {
	n, err := strconv.Atoi(s)
	if err != nil || n < -1 || (n == -1 && kind != '*' && kind != '$') {
		return 0, fmt.Errorf("bad %c length %q", kind, s)
	}
	return n, nil
}

func isAggregate(kind byte) bool { return kind == '*' || kind == '~' || kind == '>' || kind == '%' }

func (rd *reader) read() (Reply, error) {
	l, err := rd.line()
	if err != nil {
		return Reply{}, err
	}
	if len(l) == 0 {
		return Reply{}, errors.New("empty reply line")
	}
	r := Reply{Kind: l[0], Str: l[1:]}
	switch r.Kind {
	case '+', '-', ':', ',', '#', '(': // simple, error, integer, double, boolean, big number
		return r, nil
	case '_': // RESP3 null
		return r, nil
	case '$', '=': // bulk, verbatim
		return rd.bulk(r.Kind, r.Str)
	case '*', '~', '>': // array, set, push
		return rd.array(r.Kind, r.Str, 1)
	case '%': // map: n pairs, flattened like HGETALL in RESP2
		return rd.array(r.Kind, r.Str, 2)
	}
	return Reply{}, fmt.Errorf("unknown reply type %q", l)
}

func (rd *reader) bulk(kind byte, hdr string) (Reply, error) {
	n, err := length(kind, hdr)
	if err != nil {
		return Reply{}, err
	}
	if n < 0 {
		return Reply{Kind: kind, Null: true}, nil
	}
	buf := make([]byte, n+2)
	got, err := io.ReadFull(rd.r, buf)
	rd.raw = append(rd.raw, buf[:got]...)
	if err != nil {
		return Reply{}, fmt.Errorf("reading %d-byte bulk payload: %w", n, err)
	}
	if string(buf[n:]) != "\r\n" {
		return Reply{}, fmt.Errorf("bulk payload not followed by CRLF: %q", buf[n:])
	}
	return Reply{Kind: kind, Str: string(buf[:n])}, nil
}

// array reads n*per elements; -1 (null) is valid for '*' only.
func (rd *reader) array(kind byte, hdr string, per int) (Reply, error) {
	n, err := length(kind, hdr)
	if err != nil {
		return Reply{}, err
	}
	if n < 0 {
		if kind != '*' {
			return Reply{}, fmt.Errorf("bad %c length %q", kind, hdr)
		}
		return Reply{Kind: kind, Null: true}, nil
	}
	out := Reply{Kind: kind, Elems: make([]Reply, 0, n*per)}
	for i := 0; i < n*per; i++ {
		e, err := rd.read()
		if err != nil {
			return Reply{}, err
		}
		out.Elems = append(out.Elems, e)
	}
	return out, nil
}

type Conn struct {
	c  net.Conn
	rd reader
}

func Dial(addr string) (*Conn, error) {
	c, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return nil, err
	}
	return &Conn{c: c, rd: reader{r: bufio.NewReader(c)}}, nil
}

func (c *Conn) Close() { _ = c.c.Close() }

func (c *Conn) Do(args ...string) (Reply, error) {
	r, _, err := c.DoRaw(args...)
	return r, err
}

// DoRaw sends one command and returns the parsed reply plus its exact wire bytes.
func (c *Conn) DoRaw(args ...string) (Reply, []byte, error) {
	var b strings.Builder
	fmt.Fprintf(&b, "*%d\r\n", len(args))
	for _, a := range args {
		fmt.Fprintf(&b, "$%d\r\n%s\r\n", len(a), a)
	}
	_ = c.c.SetDeadline(time.Now().Add(10 * time.Second))
	if _, err := c.c.Write([]byte(b.String())); err != nil {
		return Reply{}, nil, err
	}
	c.rd.raw = c.rd.raw[:0]
	r, err := c.rd.read()
	return r, append([]byte(nil), c.rd.raw...), err
}

// AssertDrained fails if the server sent any bytes beyond the replies already read
// (an extra or duplicated reply would otherwise go unnoticed after the last command).
func (c *Conn) AssertDrained() error {
	if n := c.rd.r.Buffered(); n > 0 {
		return fmt.Errorf("%d unread bytes buffered after the last reply", n)
	}
	_ = c.c.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
	defer func() { _ = c.c.SetReadDeadline(time.Time{}) }()
	b, err := c.rd.r.Peek(1)
	if errors.Is(err, os.ErrDeadlineExceeded) {
		return nil
	}
	return fmt.Errorf("unexpected trailing data %q (err %v) after the last reply", b, err)
}

// SortElems normalises order-free replies (KEYS).
func SortElems(r Reply) Reply {
	sort.Slice(r.Elems, func(i, j int) bool { return r.Elems[i].Str < r.Elems[j].Str })
	return r
}

// PositiveAsToken normalises TTL/PTTL: a positive remaining time depends on the wall clock,
// so it compares as the token "positive"; -2/-1 (and errors) still compare exactly.
func PositiveAsToken(r Reply) Reply {
	if r.Kind == ':' {
		if n, err := strconv.ParseInt(r.Str, 10, 64); err == nil && n > 0 {
			r.Str = "positive"
		}
	}
	return r
}

// SortPairs normalises a flat field/value array (HGETALL, HRANDFIELD ... WITHVALUES) by pair.
func SortPairs(r Reply) Reply {
	if (r.Kind != '*' && r.Kind != '%') || len(r.Elems)%2 != 0 {
		return r
	}
	type pair struct{ k, v Reply }
	ps := make([]pair, 0, len(r.Elems)/2)
	for i := 0; i < len(r.Elems); i += 2 {
		ps = append(ps, pair{r.Elems[i], r.Elems[i+1]})
	}
	sort.Slice(ps, func(i, j int) bool {
		if ps[i].k.Str != ps[j].k.Str {
			return ps[i].k.Str < ps[j].k.Str
		}
		return ps[i].v.Str < ps[j].v.Str
	})
	out := Reply{Kind: r.Kind, Elems: make([]Reply, 0, len(r.Elems))}
	for _, p := range ps {
		out.Elems = append(out.Elems, p.k, p.v)
	}
	return out
}

// NormaliseScan sorts the element page of a [cursor, elements] HSCAN/SSCAN reply; the cursor
// still compares exactly.
func NormaliseScan(r Reply, pairs bool) Reply {
	if r.Kind != '*' || len(r.Elems) != 2 {
		return r
	}
	elems := append([]Reply(nil), r.Elems...) // never reorder the caller's backing array
	if pairs {
		elems[1] = SortPairs(elems[1])
	} else {
		inner := elems[1]
		inner.Elems = append([]Reply(nil), inner.Elems...)
		elems[1] = SortElems(inner)
	}
	r.Elems = elems
	return r
}

// sameShape compares random-pick replies (SRANDMEMBER/HRANDFIELD): errors and scalars exactly
// by kind/nullness, arrays by length only.
func sameShape(a, b Reply) bool {
	if a.Kind != b.Kind || a.Null != b.Null {
		return false
	}
	switch a.Kind {
	case '-':
		return a.Str == b.Str
	case '*', '~', '%', '>':
		return len(a.Elems) == len(b.Elems)
	}
	return true
}
