// TidesKV-authored (not ported): Redis 7.2 transaction semantics the Kvrocks suite does not cover.
package multi

import (
	"context"
	"testing"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/stretchr/testify/require"

	"tideskv.local/compat/util"
)

func TestMultiExtra(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()
	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()
	other := srv.NewClient()
	defer func() { require.NoError(t, other.Close()) }()

	// watchThenExec WATCHes key on a dedicated connection, runs touch, then MULTI PING EXEC.
	// It reports whether EXEC succeeded (false: null reply).
	watchThenExec := func(t *testing.T, key string, touch func()) bool {
		conn := rdb.Conn()
		defer conn.Close()
		require.NoError(t, conn.Do(ctx, "WATCH", key).Err())
		touch()
		require.NoError(t, conn.Do(ctx, "MULTI").Err())
		require.NoError(t, conn.Do(ctx, "PING").Err())
		err := conn.Do(ctx, "EXEC").Err()
		if err == redis.Nil {
			return false
		}
		require.NoError(t, err)
		return true
	}

	t.Run("No-op writes do not abort EXEC", func(t *testing.T) {
		require.NoError(t, rdb.FlushDB(ctx).Err())
		require.NoError(t, rdb.Set(ctx, "k", "v", 0).Err())
		require.NoError(t, rdb.SAdd(ctx, "s", "a").Err())
		require.True(t, watchThenExec(t, "k", func() { other.SetNX(ctx, "k", "w", 0) }))
		require.True(t, watchThenExec(t, "s", func() { other.SAdd(ctx, "s", "a") }))
		require.True(t, watchThenExec(t, "missing", func() { other.Del(ctx, "missing") }))
		require.True(t, watchThenExec(t, "k", func() { other.Persist(ctx, "k") }))
	})

	t.Run("Same-value writes and no-op LTRIM abort EXEC", func(t *testing.T) {
		require.NoError(t, rdb.FlushDB(ctx).Err())
		require.NoError(t, rdb.Set(ctx, "k", "v", 0).Err())
		require.NoError(t, rdb.RPush(ctx, "l", "a").Err())
		require.False(t, watchThenExec(t, "k", func() { other.Set(ctx, "k", "v", 0) }))
		require.False(t, watchThenExec(t, "l", func() { other.LTrim(ctx, "l", 0, -1) }))
	})

	t.Run("Watches are per database", func(t *testing.T) {
		require.NoError(t, rdb.FlushAll(ctx).Err())
		db1 := srv.NewClientWithOption(&redis.Options{DB: 1})
		defer db1.Close()
		require.True(t, watchThenExec(t, "k", func() { db1.Set(ctx, "k", "v", 0) }))
	})

	t.Run("FLUSHDB dirties only existing watched keys", func(t *testing.T) {
		require.NoError(t, rdb.FlushDB(ctx).Err())
		require.True(t, watchThenExec(t, "missing", func() { other.FlushDB(ctx) }))
		require.NoError(t, rdb.Set(ctx, "k", "v", 0).Err())
		require.False(t, watchThenExec(t, "k", func() { other.FlushDB(ctx) }))
	})

	t.Run("A live watched key that expires aborts EXEC", func(t *testing.T) {
		require.NoError(t, rdb.Set(ctx, "k", "v", 100*time.Millisecond).Err())
		require.False(t, watchThenExec(t, "k", func() { time.Sleep(300 * time.Millisecond) }))
	})

	t.Run("A key already expired at WATCH does not abort EXEC", func(t *testing.T) {
		require.NoError(t, rdb.Set(ctx, "k", "v", 50*time.Millisecond).Err())
		time.Sleep(100 * time.Millisecond)
		require.True(t, watchThenExec(t, "k", func() { other.Del(ctx, "k") }))
	})

	t.Run("A served blocked pop aborts EXEC of a watcher", func(t *testing.T) {
		require.NoError(t, rdb.FlushDB(ctx).Err())
		waiter := srv.NewClient()
		defer waiter.Close()
		done := make(chan struct{})
		go func() {
			defer close(done)
			waiter.BLPop(ctx, 0, "q")
		}()
		require.Eventually(t, func() bool { return util.FindInfoEntry(rdb, "blocked_clients") == "1" },
			5*time.Second, 10*time.Millisecond)
		require.False(t, watchThenExec(t, "q", func() {
			other.LPush(ctx, "q", "x") // served at once: q is created and popped
			<-done
		}))
	})

	t.Run("Blocking commands inside MULTI answer null at once", func(t *testing.T) {
		require.NoError(t, rdb.FlushDB(ctx).Err())
		c := srv.NewTCPClient()
		defer c.Close()
		require.NoError(t, c.WriteArgs("MULTI"))
		c.MustRead(t, "+OK")
		for _, args := range [][]string{{"BLMOVE", "q", "d", "LEFT", "LEFT", "0"}, {"BZPOPMIN", "z", "0"},
			{"BLMPOP", "0", "1", "q", "LEFT"}} {
			require.NoError(t, c.WriteArgs(args...))
			c.MustRead(t, "+QUEUED")
		}
		require.NoError(t, c.WriteArgs("EXEC"))
		c.MustRead(t, "*3")
		c.MustRead(t, "$-1")
		c.MustRead(t, "*-1")
		c.MustRead(t, "*-1")
	})

	t.Run("WRONGTYPE inside EXEC is an element and other commands apply", func(t *testing.T) {
		require.NoError(t, rdb.FlushDB(ctx).Err())
		require.NoError(t, rdb.Set(ctx, "s", "str", 0).Err())
		cmds, err := rdb.TxPipelined(ctx, func(p redis.Pipeliner) error {
			p.LPush(ctx, "s", "x")
			p.Set(ctx, "a", "1", 0)
			return nil
		})
		require.Error(t, err)
		util.ErrorRegexp(t, cmds[0].Err(), "^WRONGTYPE")
		require.Equal(t, "1", rdb.Get(ctx, "a").Val())
	})

	t.Run("SELECT inside EXEC persists", func(t *testing.T) {
		c := srv.NewTCPClient()
		defer c.Close()
		for _, args := range [][]string{{"MULTI"}, {"SELECT", "5"}, {"SET", "k", "db5"}, {"EXEC"}} {
			require.NoError(t, c.WriteArgs(args...))
		}
		c.MustRead(t, "+OK")
		c.MustRead(t, "+QUEUED")
		c.MustRead(t, "+QUEUED")
		c.MustRead(t, "*2")
		c.MustRead(t, "+OK")
		c.MustRead(t, "+OK")
		require.NoError(t, c.WriteArgs("GET", "k"))
		c.MustRead(t, "$3")
		c.MustRead(t, "db5")
	})

	t.Run("Rejected EXEC names the reason", func(t *testing.T) {
		c := srv.NewTCPClient()
		defer c.Close()
		require.NoError(t, c.WriteArgs("EXEC", "x"))
		c.MustRead(t, "-EXECABORT Transaction discarded because of: wrong number of arguments for 'exec' command")
	})

	t.Run("Pipelined MULTI with a bad command still answers QUEUED", func(t *testing.T) {
		c := srv.NewTCPClient()
		defer c.Close()
		require.NoError(t, c.Write("MULTI\r\nSET a\r\nSET b 1\r\nEXEC\r\n")) // inline commands
		c.MustRead(t, "+OK")
		c.MustMatch(t, "^-ERR wrong number of arguments")
		c.MustRead(t, "+QUEUED")
		c.MustRead(t, "-EXECABORT Transaction discarded because of previous errors.")
	})
}
