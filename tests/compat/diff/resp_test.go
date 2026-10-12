package diff

import (
	"bufio"
	"io"
	"net"
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
		"unknown type":             "%1\r\n",
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
