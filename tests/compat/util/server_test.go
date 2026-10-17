package util

import (
	"strconv"
	"testing"

	"github.com/stretchr/testify/require"
)

// FreePort's probe socket is closed before the server binds, so a parallel package may take the port first.
// StartServer must then run its own server on another port, never hand the test the server that holds it.
func TestStartServerRetriesOnAPortAnotherServerHolds(t *testing.T) {
	holder := StartServer(t, map[string]string{})
	defer holder.Close()
	taken := holder.addr.Port
	calls := 0
	pickPort = func(t testing.TB) int {
		calls++
		if calls == 1 {
			return taken
		}
		return freePort(t)
	}
	defer func() { pickPort = freePort }()

	srv := StartServer(t, map[string]string{})
	defer srv.Close()
	require.NotEqual(t, taken, srv.addr.Port)
	c := srv.NewClient()
	defer func() { require.NoError(t, c.Close()) }()
	require.Equal(t, strconv.Itoa(srv.proc.cmd.Process.Pid), FindInfoEntry(c, "process_id"))
}
