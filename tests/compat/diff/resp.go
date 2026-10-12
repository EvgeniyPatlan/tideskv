// Package diff runs the same random command stream against tideskv and Redis 7.2 and
// compares every reply exactly (spec §10.3 differential tests).
package diff

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Reply is a parsed RESP2 reply. Kind is the RESP type byte: + - : $ *.
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
	case r.Kind == '*':
		parts := make([]string, len(r.Elems))
		for i, e := range r.Elems {
			parts[i] = e.String()
		}
		return "[" + strings.Join(parts, ", ") + "]"
	default:
		return string(r.Kind) + strconv.Quote(r.Str)
	}
}

type Conn struct {
	c net.Conn
	r *bufio.Reader
}

func Dial(addr string) (*Conn, error) {
	c, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return nil, err
	}
	return &Conn{c: c, r: bufio.NewReader(c)}, nil
}

func (c *Conn) Close() { _ = c.c.Close() }

func (c *Conn) Do(args ...string) (Reply, error) {
	var b strings.Builder
	fmt.Fprintf(&b, "*%d\r\n", len(args))
	for _, a := range args {
		fmt.Fprintf(&b, "$%d\r\n%s\r\n", len(a), a)
	}
	_ = c.c.SetDeadline(time.Now().Add(10 * time.Second))
	if _, err := c.c.Write([]byte(b.String())); err != nil {
		return Reply{}, err
	}
	return c.read()
}

func (c *Conn) line() (string, error) {
	s, err := c.r.ReadString('\n')
	return strings.TrimSuffix(s, "\r\n"), err
}

func (c *Conn) read() (Reply, error) {
	l, err := c.line()
	if err != nil || len(l) == 0 {
		return Reply{}, fmt.Errorf("bad reply line %q: %v", l, err)
	}
	r := Reply{Kind: l[0], Str: l[1:]}
	switch r.Kind {
	case '+', '-', ':':
		return r, nil
	case '$':
		n, _ := strconv.Atoi(r.Str)
		if n < 0 {
			return Reply{Kind: '$', Null: true}, nil
		}
		buf := make([]byte, n+2)
		if _, err := io.ReadFull(c.r, buf); err != nil {
			return Reply{}, err
		}
		return Reply{Kind: '$', Str: string(buf[:n])}, nil
	case '*':
		n, _ := strconv.Atoi(r.Str)
		if n < 0 {
			return Reply{Kind: '*', Null: true}, nil
		}
		out := Reply{Kind: '*', Elems: make([]Reply, 0, n)}
		for i := 0; i < n; i++ {
			e, err := c.read()
			if err != nil {
				return Reply{}, err
			}
			out.Elems = append(out.Elems, e)
		}
		return out, nil
	}
	return Reply{}, fmt.Errorf("unknown reply type %q", l)
}

// SortElems normalises order-free replies (KEYS).
func SortElems(r Reply) Reply {
	sort.Slice(r.Elems, func(i, j int) bool { return r.Elems[i].Str < r.Elems[j].Str })
	return r
}
