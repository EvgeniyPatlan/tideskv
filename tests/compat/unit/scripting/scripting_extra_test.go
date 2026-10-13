// TidesKV-authored (not ported): Redis 7.2 scripting semantics the Kvrocks suite does not cover.
package scripting

import (
	"context"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/stretchr/testify/require"

	"tideskv.local/compat/util"
)

// evalErr runs EVAL with numkeys=0 plus extra args and returns the error text ("" when there is none).
func evalErr(ctx context.Context, rdb *redis.Client, script string, args ...interface{}) string {
	cmd := append([]interface{}{"EVAL", script, 0}, args...)
	if err := rdb.Do(ctx, cmd...).Err(); err != nil {
		return err.Error()
	}
	return ""
}

func TestScriptingWireFormat(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()
	c := srv.NewTCPClient()
	defer func() { require.NoError(t, c.Close()) }()

	// send runs one EVAL on the raw connection and returns the first reply line.
	send := func(t *testing.T, script string) string {
		require.NoError(t, c.WriteArgs("EVAL", script, "0"))
		line, err := c.ReadLine()
		require.NoError(t, err)
		return line
	}

	t.Run("Status replies are simple strings on the wire, not bulk strings", func(t *testing.T) {
		require.Equal(t, "+OK", send(t, `return redis.call('set','k','v')`))
		require.Equal(t, "+x", send(t, `return {ok='x'}`))
	})

	t.Run("status_reply and error_reply wire bytes", func(t *testing.T) {
		require.Equal(t, "+FINE", send(t, `return redis.status_reply('FINE')`))
		require.Equal(t, "-MY ERR", send(t, `return redis.error_reply('MY ERR')`))
		require.Equal(t, "-MY_ERR_CODE custom msg", send(t, `return redis.error_reply('MY_ERR_CODE custom msg')`))
		require.Equal(t, "-ERR no_code", send(t, `return redis.error_reply('no_code')`))
		require.Equal(t, "-this is an error", send(t, `return {err='this is an error'}`))
		// error_reply with a bad argument does not raise; it returns an error table
		require.Equal(t, "-ERR wrong number or type of arguments", send(t, `return redis.error_reply()`))
		require.Equal(t, "-ERR wrong number or type of arguments", send(t, `return redis.status_reply({})`))
		require.Equal(t, "+OK", send(t, `return redis.call('ping') and redis.status_reply('OK')`))
	})

	t.Run("Nil stops a table reply, false does not; large numbers keep their precision", func(t *testing.T) {
		require.Equal(t, "*1", send(t, `return {1,nil,3}`))
		c.MustRead(t, ":1")
		require.Equal(t, "*3", send(t, `return {1,false,3}`))
		c.MustRead(t, ":1")
		c.MustRead(t, "$-1")
		c.MustRead(t, ":3")
		require.Equal(t, ":9007199254740991", send(t, `return 9007199254740991`))
		require.Equal(t, "+OK", send(t, `return redis.call('set','foo',9007199254740991)`))
		require.Equal(t, "$16", send(t, `return redis.call('get','foo')`))
		c.MustRead(t, "9007199254740991")
		require.NoError(t, c.WriteArgs("EVAL", `redis.call('set','big',ARGV[1]) return redis.call('get','big')`, "0", "12039611435714932082"))
		c.MustRead(t, "$20")
		c.MustRead(t, "12039611435714932082")
	})

	t.Run("Recursive table reply ends in an error element and the connection survives", func(t *testing.T) {
		require.NoError(t, c.WriteArgs("EVAL", `local a={} local b={a} a[1]=b return a`, "0"))
		deadline := time.Now().Add(10 * time.Second)
		for {
			line, err := c.ReadLine()
			require.NoError(t, err)
			if line == "-ERR reached lua stack limit" {
				break
			}
			require.Equal(t, "*1", line)
			require.True(t, time.Now().Before(deadline))
		}
		require.NoError(t, c.WriteArgs("PING"))
		c.MustRead(t, "+PONG")
	})
}

func TestScriptingErrors(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()
	ctx := context.Background()
	rdb := srv.NewClientWithOption(&redis.Options{Protocol: 2})
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("WRONGTYPE keeps its code and gets the script suffix", func(t *testing.T) {
		require.NoError(t, rdb.Set(ctx, "str", "v", 0).Err())
		err := evalErr(ctx, rdb, `redis.call('lpush','str','x')`)
		require.Regexp(t, `^WRONGTYPE .* script: [0-9a-f]{40}, on @user_script:1\.$`, err)

		// a returned pcall error carries no suffix
		err = evalErr(ctx, rdb, `return redis.pcall('lpush','str','x')`)
		require.Regexp(t, `^WRONGTYPE Operation against a key holding the wrong kind of value$`, err)
	})

	t.Run("Raised errors end with the script suffix; the shebang line keeps its line number", func(t *testing.T) {
		require.Regexp(t, `^ERR Unknown Redis command called from script script: [0-9a-f]{40}, on @user_script:1\.$`,
			evalErr(ctx, rdb, `redis.call('nosuchcommand')`))
		require.Regexp(t, `^ERR Unknown Redis command called from script script: [0-9a-f]{40}, on @user_script:3\.$`,
			evalErr(ctx, rdb, "local x = 1\nlocal y = 2\nredis.call('nosuchcommand')"))
		require.Regexp(t, `^ERR Unknown Redis command called from script script: [0-9a-f]{40}, on @user_script:2\.$`,
			evalErr(ctx, rdb, "#!lua\nredis.call('nosuchcommand')"))
		require.Regexp(t, `^ERR user_script:1: boom script: [0-9a-f]{40}, on @user_script:1\.$`, evalErr(ctx, rdb, `error('boom')`))
		require.Regexp(t, `^ERR boom script: [0-9a-f]{40}, on @user_script:1\.$`, evalErr(ctx, rdb, `error('boom', 0)`))
		require.Regexp(t, `^MY boom script: [0-9a-f]{40}, on @user_script:1\.$`, evalErr(ctx, rdb, `error({err='MY boom'})`))
		require.Regexp(t, `^ERR Wrong number of args calling Redis command from script script: `,
			evalErr(ctx, rdb, `redis.call('get')`))
		require.Regexp(t, `^ERR Lua redis lib command arguments must be strings or integers script: `,
			evalErr(ctx, rdb, `redis.call('get', {})`))
		// the failed script does not poison the next one
		require.Equal(t, "", evalErr(ctx, rdb, `return 1`))
	})

	t.Run("NOSCRIPT text, compile errors and uncached failures", func(t *testing.T) {
		const noscript = "NOSCRIPT No matching script. Please use EVAL."
		err := rdb.EvalSha(ctx, "ffd632c7d33e571e9f24556ebed26c3479a87130", nil).Err()
		require.EqualError(t, err, noscript)
		require.EqualError(t, rdb.EvalSha(ctx, "short", nil).Err(), noscript)

		compile := evalErr(ctx, rdb, `local a b`)
		require.Regexp(t, `^ERR Error compiling script \(new function\): user_script:1: .*near`, compile)
		loadErr := rdb.Do(ctx, "SCRIPT", "LOAD", `local a b`).Err()
		require.Regexp(t, `^ERR Error compiling script \(new function\): user_script:1: `, loadErr.Error())
		// neither the failed EVAL nor the failed SCRIPT LOAD cached the body
		failedSha := sha1hex(ctx, t, rdb, `local a b`)
		exists, err := rdb.ScriptExists(ctx, failedSha).Result()
		require.NoError(t, err)
		require.Equal(t, []bool{false}, exists)
		require.EqualError(t, rdb.EvalSha(ctx, failedSha, nil).Err(), noscript)
	})

	t.Run("numkeys and arity errors", func(t *testing.T) {
		require.EqualError(t, rdb.Do(ctx, "EVAL", "return 1", "notanumber").Err(), "ERR value is not an integer or out of range")
		require.EqualError(t, rdb.Do(ctx, "EVAL", "return 1", "5", "a").Err(), "ERR Number of keys can't be greater than number of args")
		require.EqualError(t, rdb.Do(ctx, "EVAL", "return 1", "-1").Err(), "ERR Number of keys can't be negative")
		require.EqualError(t, rdb.Do(ctx, "EVAL", "return 1").Err(), "ERR wrong number of arguments for 'eval' command")
		require.EqualError(t, rdb.Do(ctx, "EVALSHA", "x").Err(), "ERR wrong number of arguments for 'evalsha' command")
		// an empty script is valid and returns nil
		require.EqualError(t, rdb.Do(ctx, "EVAL", "", "0").Err(), redis.Nil.Error())
		// the EVALSHA length check wins over the numkeys check
		require.EqualError(t, rdb.Do(ctx, "EVALSHA", "short", "bogus").Err(), "NOSCRIPT No matching script. Please use EVAL.")
	})

	t.Run("luaL_error style errors carry the user_script:N: position prefix", func(t *testing.T) {
		const suffix = ` script: [0-9a-f]{40}, on @user_script:1\.$`
		require.Regexp(t, `^ERR user_script:1: Script attempted to access nonexistent global variable 'x'`+suffix,
			evalErr(ctx, rdb, `return x`))
		require.Regexp(t, `^ERR user_script:1: Script attempted to access nonexistent global variable 'print'`+suffix,
			evalErr(ctx, rdb, `return print`))
		require.Regexp(t, `^ERR user_script:1: wrong number of arguments`+suffix, evalErr(ctx, rdb, `return math.random(1,2,3)`))
		require.Regexp(t, `^ERR user_script:1: bad argument #1 to 'random' \(interval is empty\)`+suffix,
			evalErr(ctx, rdb, `return math.random(0)`))
		require.Regexp(t, `^ERR user_script:1: bad argument #2 to 'random' \(interval is empty\)`+suffix,
			evalErr(ctx, rdb, `return math.random(2,1)`))
		require.Regexp(t, `^ERR user_script:1: bad argument #1 to 'randomseed' \(number expected, got string\)`+suffix,
			evalErr(ctx, rdb, `math.randomseed('x')`))
		// helpers that push an error table (luaPushError) have no position prefix
		require.Regexp(t, `^ERR wrong number of arguments`+suffix, evalErr(ctx, rdb, `return redis.sha1hex()`))
		// the readonly-table error comes from the VM and has the prefix
		require.Regexp(t, `^ERR user_script:1: Attempt to modify a readonly table`+suffix, evalErr(ctx, rdb, `x=1`))
	})

	t.Run("Lua library argument errors", func(t *testing.T) {
		require.Regexp(t, `^ERR wrong number of arguments script: `, evalErr(ctx, rdb, `return redis.sha1hex()`))
		require.Regexp(t, `^ERR redis.setresp\(\) requires one argument\. script: `, evalErr(ctx, rdb, `redis.setresp()`))
		require.Regexp(t, `^ERR Please specify at least one argument for this redis lib call script: `, evalErr(ctx, rdb, `redis.call()`))
		// pcall with no arguments returns the error table without raising
		require.Equal(t, "ERR Please specify at least one argument for this redis lib call", evalErr(ctx, rdb, `return redis.pcall()`))
	})
}

func TestScriptingCache(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()
	ctx := context.Background()
	a := srv.NewClient()
	defer func() { require.NoError(t, a.Close()) }()

	const body = `return 'cached'`

	t.Run("EVAL populates a cache that every connection sees", func(t *testing.T) {
		sha := sha1hex(ctx, t, a, body)
		require.Equal(t, "cached", a.Eval(ctx, body, nil).Val())
		for i := 0; i < 8; i++ {
			other := srv.NewClient()
			require.Equal(t, "cached", other.EvalSha(ctx, sha, nil).Val(), "connection %d", i)
			require.Equal(t, []bool{true}, other.ScriptExists(ctx, sha).Val())
			require.NoError(t, other.Close())
		}
	})

	t.Run("SCRIPT LOAD, EVALSHA and SCRIPT FLUSH cross connections", func(t *testing.T) {
		b := srv.NewClient()
		defer func() { require.NoError(t, b.Close()) }()
		loaded := a.ScriptLoad(ctx, `return 'shared'`).Val()
		require.Equal(t, "shared", b.EvalSha(ctx, loaded, nil).Val())
		require.NoError(t, b.ScriptFlush(ctx).Err())
		require.Equal(t, []bool{false}, a.ScriptExists(ctx, loaded).Val())
		require.Regexp(t, `^NOSCRIPT`, a.EvalSha(ctx, loaded, nil).Err().Error())
	})

	t.Run("SCRIPT FLUSH modes, EXISTS arity, HELP and unknown subcommands", func(t *testing.T) {
		require.Equal(t, "OK", a.Do(ctx, "SCRIPT", "FLUSH", "ASYNC").Val())
		require.Equal(t, "OK", a.Do(ctx, "SCRIPT", "FLUSH", "SYNC").Val())
		require.Equal(t, "OK", a.Do(ctx, "SCRIPT", "FLUSH", "async").Val())
		require.EqualError(t, a.Do(ctx, "SCRIPT", "FLUSH", "bogus").Err(), "ERR SCRIPT FLUSH only support SYNC|ASYNC option")
		require.EqualError(t, a.Do(ctx, "SCRIPT", "FLUSH", "SYNC", "ASYNC").Err(), "ERR SCRIPT FLUSH only support SYNC|ASYNC option")
		require.EqualError(t, a.Do(ctx, "SCRIPT", "EXISTS").Err(), "ERR wrong number of arguments for 'script|exists' command")
		require.EqualError(t, a.Do(ctx, "SCRIPT").Err(), "ERR wrong number of arguments for 'script' command")
		require.EqualError(t, a.Do(ctx, "SCRIPT", "FOO").Err(), "ERR unknown subcommand 'FOO'. Try SCRIPT HELP.")
		// SCRIPT EXISTS accepts any number of hashes of any length and is case-insensitive
		sha := a.ScriptLoad(ctx, `return 1`).Val()
		require.Equal(t, []bool{true, false, true}, a.ScriptExists(ctx, sha, "nope", strings.ToUpper(sha)).Val())

		help, err := a.Do(ctx, "SCRIPT", "HELP").Slice()
		require.NoError(t, err)
		require.Len(t, help, 17)
		require.Equal(t, "SCRIPT <subcommand> [<arg> [value] [opt] ...]. Subcommands are:", help[0])
		require.Equal(t, "    Print this help.", help[16])
	})

	t.Run("A shebang changes the script hash", func(t *testing.T) {
		plain := sha1hex(ctx, t, a, "return 1")
		shebang := sha1hex(ctx, t, a, "#!lua\nreturn 1")
		require.NotEqual(t, plain, shebang)
		require.Equal(t, shebang, a.ScriptLoad(ctx, "#!lua\nreturn 1").Val())
	})
}

// sha1hex asks the server for the SHA1 of body via redis.sha1hex.
func sha1hex(ctx context.Context, t *testing.T, rdb *redis.Client, body string) string {
	r := rdb.Eval(ctx, `return redis.sha1hex(ARGV[1])`, nil, body)
	require.NoError(t, r.Err())
	return r.Val().(string)
}

func TestScriptingSandbox(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()
	ctx := context.Background()
	rdb := srv.NewClientWithOption(&redis.Options{Protocol: 2})
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Globals and library tables are read-only and mutations never leak", func(t *testing.T) {
		const readonly = `Attempt to modify a readonly table`
		for _, script := range []string{
			`a=10`,
			`_G.x=1`,
			`redis.call=nil`,
			`string.foo=1`,
			`cjson.encode=nil`,
			`setmetatable(_G, {})`,
			`getmetatable(_G).__index={}`,
			`redis=function() end`,
			`_G={}`,
			`rawset(_G, 'x', 1)`,
		} {
			require.Contains(t, evalErr(ctx, rdb, script), readonly, script)
		}
		// no earlier attempt changed anything a later script can see
		require.Equal(t, "PONG", rdb.Eval(ctx, `return redis.call('ping')`, nil).Val())
		require.Equal(t, "abc", rdb.Eval(ctx, `return string.rep('abc', 1)`, nil).Val())
		require.Contains(t, evalErr(ctx, rdb, `return x`), "nonexistent global variable 'x'")
	})

	t.Run("KEYS and ARGV are fresh for every run", func(t *testing.T) {
		require.Equal(t, []interface{}{"k1", "a1"}, rdb.Eval(ctx, `return {KEYS[1], ARGV[1]}`, []string{"k1"}, "a1").Val())
		require.Equal(t, int64(0), rdb.Eval(ctx, `return #KEYS + #ARGV`, nil).Val())
		require.Equal(t, int64(1), rdb.Eval(ctx, `KEYS[1] = 'poison' return #KEYS`, nil).Val())
		require.Equal(t, int64(0), rdb.Eval(ctx, `return #KEYS`, nil).Val())
		require.Equal(t, []interface{}{"b"}, rdb.Eval(ctx, `return {ARGV[1]}`, nil, "b").Val())
		// ARGV elements are strings even for numeric input
		require.Equal(t, "string", rdb.Eval(ctx, `return type(ARGV[1])`, nil, 5).Val())
	})

	t.Run("Unavailable builtins raise a nonexistent-global error", func(t *testing.T) {
		for _, name := range []string{"print", "loadfile", "dofile", "os", "io", "require", "package", "debug", "module"} {
			require.Contains(t, evalErr(ctx, rdb, "return "+name), "nonexistent global variable '"+name+"'", name)
		}
		// tideskv: getfenv/setfenv/newproxy are absent from the Redis 7.2.16 sandbox (7.2.5 still has them)
		for _, name := range []string{"getfenv", "setfenv", "newproxy"} {
			require.Contains(t, evalErr(ctx, rdb, "return "+name), "nonexistent global variable '"+name+"'", name)
		}
		// everything else the sandbox allows is still reachable
		for _, name := range []string{"loadstring", "load", "gcinfo", "coroutine", "pcall", "xpcall", "unpack", "tostring"} {
			require.Equal(t, "", evalErr(ctx, rdb, "return type("+name+")"), name)
		}
	})

	t.Run("Binary chunks are refused", func(t *testing.T) {
		require.Equal(t, "nil", rdb.Eval(ctx, `return type(loadstring(string.dump(function() return 1 end)))`, nil).Val())
		require.Equal(t, "nil", rdb.Eval(ctx, `return type(loadstring("\27Lua\81\0\1\4\8\4\8\0"))`, nil).Val())
		require.Contains(t, evalErr(ctx, rdb, `loadstring(string.dump(function() return 1 end))()`), "attempt to call a nil value")
		require.NoError(t, rdb.Ping(ctx).Err())
	})

	t.Run("redis.sha1hex vectors", func(t *testing.T) {
		require.Equal(t, "da39a3ee5e6b4b0d3255bfef95601890afd80709", rdb.Eval(ctx, `return redis.sha1hex('')`, nil).Val())
		require.Equal(t, "74822d82031af7493c20eefa13bd07ec4fada82f", rdb.Eval(ctx, `return redis.sha1hex('Pizza & Mandolino')`, nil).Val())
		require.Contains(t, evalErr(ctx, rdb, `return redis.sha1hex()`), "wrong number of arguments")
		require.Contains(t, evalErr(ctx, rdb, `return redis.sha1hex('a', 'b')`), "wrong number of arguments")
	})

	t.Run("redis.REDIS_VERSION, replicate_commands and set_repl", func(t *testing.T) {
		// tideskv: TidesKV reports exactly "7.2.0" (ruled value); live Redis 7.2.5 reports "7.2.5"
		require.Equal(t, "7.2.0", rdb.Eval(ctx, `return redis.REDIS_VERSION`, nil).Val())
		num := rdb.Eval(ctx, `return redis.REDIS_VERSION_NUM`, nil).Val().(int64)
		require.GreaterOrEqual(t, num, int64(0x070200))
		require.Less(t, num, int64(0x070300))
		require.Equal(t, int64(1), rdb.Eval(ctx, `return redis.replicate_commands()`, nil).Val())
		require.EqualError(t, rdb.Eval(ctx, `return redis.set_repl(redis.REPL_ALL)`, nil).Err(), redis.Nil.Error())
		require.Contains(t, evalErr(ctx, rdb, `redis.set_repl(12345)`), "Invalid replication flags")
		require.Contains(t, evalErr(ctx, rdb, `redis.set_repl()`), "redis.set_repl() requires one argument.")
		require.EqualError(t, rdb.Eval(ctx, `return redis.breakpoint()`, nil).Err(), redis.Nil.Error())
	})

	t.Run("cjson, cmsgpack, struct and bit smoke tests", func(t *testing.T) {
		require.Equal(t, "[1,2,3]", rdb.Eval(ctx, `return cjson.encode(cjson.decode('[1,2,3]'))`, nil).Val())
		require.Equal(t, int64(1), rdb.Eval(ctx, `return cjson.decode('{"a":null}').a == cjson.null`, nil).Val())
		require.Equal(t, `{"a":[1,2]}`, rdb.Eval(ctx, `return cjson.encode({a={1,2}})`, nil).Val())
		require.Contains(t, evalErr(ctx, rdb, `return cjson.decode('{')`), "Expected object key string")
		require.Contains(t, evalErr(ctx, rdb, `cjson.decode(string.rep('[', 2000))`), "Found too many nested data structures")
		require.Equal(t, "hello", rdb.Eval(ctx, `return cmsgpack.unpack(cmsgpack.pack('hello'))`, nil).Val())
		require.Equal(t, []interface{}{int64(1), int64(2)}, rdb.Eval(ctx, `return {cmsgpack.unpack(cmsgpack.pack(1, 2))}`, nil).Val())
		require.Equal(t, []interface{}{int64(258), int64(7)},
			rdb.Eval(ctx, `local a, b = struct.unpack('>HB', struct.pack('>HB', 258, 7)) return {a, b}`, nil).Val())
		require.Equal(t, int64(4), rdb.Eval(ctx, `return struct.size('>I4')`, nil).Val())
		require.Equal(t, "000000ff", rdb.Eval(ctx, `return bit.tohex(255)`, nil).Val())
		require.Equal(t, int64(-1), rdb.Eval(ctx, `return bit.bnot(0)`, nil).Val())
		require.Equal(t, int64(256), rdb.Eval(ctx, `return bit.lshift(1, 8)`, nil).Val())
		require.Equal(t, int64(1), rdb.Eval(ctx, `return bit.rshift(256, 8)`, nil).Val())
	})
}

func TestScriptingKeyspaceInteraction(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()
	ctx := context.Background()
	rdb := srv.NewClientWithOption(&redis.Options{Protocol: 2})
	defer func() { require.NoError(t, rdb.Close()) }()
	require.NoError(t, rdb.FlushAll(ctx).Err())

	t.Run("SELECT inside a script does not leak, and the script starts in the caller's db", func(t *testing.T) {
		db3 := srv.NewClientWithOption(&redis.Options{Protocol: 2, DB: 3})
		defer func() { require.NoError(t, db3.Close()) }()
		require.NoError(t, db3.Set(ctx, "k3", "in3", 0).Err())
		require.Equal(t, "in3", db3.Eval(ctx, `return redis.call('get', 'k3')`, nil).Val())
		require.Equal(t, "5", db3.Eval(ctx, `redis.call('select', 5) redis.call('set', 'k5', 'in5') return '5'`, nil).Val())
		// the caller is still in db 3 and the next script starts there again
		require.Equal(t, "in3", db3.Get(ctx, "k3").Val())
		require.EqualError(t, db3.Get(ctx, "k5").Err(), redis.Nil.Error())
		require.Equal(t, "in3", db3.Eval(ctx, `return redis.call('get', 'k3')`, nil).Val())
		require.Equal(t, "in5", rdb.Do(ctx, "EVAL", `redis.call('select', 5) return redis.call('get', 'k5')`, 0).Val())
		require.Contains(t, evalErr(ctx, rdb, `redis.call('select', 9999)`), "ERR DB index is out of range")
	})

	t.Run("Every blocking command returns nil at once inside a script", func(t *testing.T) {
		tctx, cancel := context.WithTimeout(ctx, 5*time.Second)
		defer cancel()
		for _, cmd := range []string{
			`'blpop', 'bk1', 0`,
			`'brpop', 'bk1', 0`,
			`'blmove', 'bk1', 'bk2', 'left', 'right', 0`,
			`'brpoplpush', 'bk1', 'bk2', 0`,
			`'blmpop', 0, 1, 'bk1', 'left'`,
			`'bzpopmin', 'bz1', 0`,
			`'bzpopmax', 'bz1', 0`,
			`'bzmpop', 0, 1, 'bz1', 'min'`,
		} {
			r := rdb.Do(tctx, "EVAL", fmt.Sprintf(`return redis.pcall(%s)`, cmd), 0)
			require.EqualError(t, r.Err(), redis.Nil.Error(), cmd)
		}
		// with data present they pop normally
		require.NoError(t, rdb.RPush(tctx, "bk1", "x").Err())
		require.Equal(t, []interface{}{"bk1", "x"}, rdb.Eval(tctx, `return redis.call('blpop', 'bk1', 0)`, nil).Val())
	})

	t.Run("EVAL inside MULTI runs at EXEC and its error stays inside the EXEC array", func(t *testing.T) {
		conn := rdb.Conn()
		defer conn.Close()
		require.NoError(t, conn.Do(ctx, "MULTI").Err())
		require.Equal(t, "QUEUED", conn.Do(ctx, "EVAL", `return redis.call('incr', 'mk')`, 0).Val())
		require.Equal(t, "QUEUED", conn.Do(ctx, "EVAL", `return redis.call('incr', 'mk')`, 0).Val())
		require.Equal(t, "QUEUED", conn.Do(ctx, "EVAL", `redis.call('nosuchcommand')`, 0).Val())
		res, err := conn.Do(ctx, "EXEC").Slice()
		require.NoError(t, err)
		require.Equal(t, int64(1), res[0])
		require.Equal(t, int64(2), res[1])
		require.Regexp(t, `^ERR Unknown Redis command called from script`, fmt.Sprint(res[2]))
		// the arity of a queued EVAL is checked at queue time
		require.NoError(t, conn.Do(ctx, "MULTI").Err())
		require.Regexp(t, `wrong number of arguments for 'eval' command`, conn.Do(ctx, "EVAL", "x").Err().Error())
		require.NoError(t, conn.Do(ctx, "DISCARD").Err())
	})

	t.Run("A script write dirties a WATCH; a no-op write does not", func(t *testing.T) {
		watcher := rdb.Conn()
		defer watcher.Close()
		watchExec := func(touch func()) bool {
			require.NoError(t, watcher.Do(ctx, "WATCH", "wk").Err())
			touch()
			require.NoError(t, watcher.Do(ctx, "MULTI").Err())
			require.NoError(t, watcher.Do(ctx, "PING").Err())
			err := watcher.Do(ctx, "EXEC").Err()
			if err == redis.Nil {
				return false
			}
			require.NoError(t, err)
			return true
		}
		require.NoError(t, rdb.Set(ctx, "wk", "v", 0).Err())
		require.False(t, watchExec(func() { require.NoError(t, rdb.Eval(ctx, `return redis.call('set', 'wk', 'w')`, nil).Err()) }))
		require.True(t, watchExec(func() { require.NoError(t, rdb.Eval(ctx, `return redis.call('setnx', 'wk', 'z')`, nil).Err()) }))
		require.True(t, watchExec(func() { require.NoError(t, rdb.Eval(ctx, `return redis.call('del', 'absent')`, nil).Err()) }))
		require.True(t, watchExec(func() { require.NoError(t, rdb.Eval(ctx, `return redis.call('get', 'wk')`, nil).Err()) }))
		require.False(t, watchExec(func() { require.NoError(t, rdb.Eval(ctx, `return redis.call('del', 'wk')`, nil).Err()) }))
	})
}

func TestScriptingCommandRestrictions(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()
	ctx := context.Background()
	rdb := srv.NewClientWithOption(&redis.Options{Protocol: 2})
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Commands that are not allowed from a script", func(t *testing.T) {
		for _, cmd := range []string{
			`'multi'`, `'exec'`, `'watch', 'k'`, `'unwatch'`, `'discard'`, `'eval', 'return 1', 0`,
			`'script', 'flush'`, `'quit'`, `'reset'`, `'client', 'id'`, `'config', 'get', 'port'`,
		} {
			r := rdb.Eval(ctx, fmt.Sprintf(`return redis.pcall(%s)`, cmd), nil)
			require.EqualError(t, r.Err(), "ERR This Redis command is not allowed from script", cmd)
			// redis.call raises the same error with the script suffix
			require.Regexp(t, `^ERR This Redis command is not allowed from script script: [0-9a-f]{40}, on @user_script:1\.$`,
				evalErr(ctx, rdb, fmt.Sprintf(`redis.call(%s)`, cmd)), cmd)
		}
		// the arity check runs before the not-allowed check
		require.Regexp(t, `^ERR Wrong number of args calling Redis command from script`, evalErr(ctx, rdb, `redis.call('watch')`))
		// and the connection is still fine
		require.NoError(t, rdb.Ping(ctx).Err())
	})

	t.Run("An error halfway does not roll earlier writes back", func(t *testing.T) {
		require.NoError(t, rdb.Del(ctx, "a", "cnt").Err())
		err := evalErr(ctx, rdb, `redis.call('set','a',1) redis.call('lpush','a','x')`)
		require.Regexp(t, `^WRONGTYPE `, err)
		require.Equal(t, "1", rdb.Get(ctx, "a").Val())

		err = evalErr(ctx, rdb, `redis.call('incr','cnt') redis.call('nonexisting')`)
		require.Regexp(t, `^ERR Unknown Redis command`, err)
		require.Equal(t, "1", rdb.Get(ctx, "cnt").Val())

		// redis.pcall keeps going after a failing command
		r := rdb.Eval(ctx, `redis.pcall('lpush','a','x') redis.call('incr','cnt') return redis.call('get','cnt')`, nil)
		require.NoError(t, r.Err())
		require.Equal(t, "2", r.Val())
	})

	t.Run("EVAL_RO, EVALSHA_RO and the shebang flags", func(t *testing.T) {
		require.NoError(t, rdb.Set(ctx, "ro", "v", 0).Err())
		require.Equal(t, "v", rdb.Do(ctx, "EVAL_RO", `return redis.call('get', KEYS[1])`, 1, "ro").Val())
		sha := rdb.ScriptLoad(ctx, `return redis.call('get', KEYS[1])`).Val()
		require.Equal(t, "v", rdb.Do(ctx, "EVALSHA_RO", sha, 1, "ro").Val())
		require.Equal(t, "v", rdb.Do(ctx, "EVALSHA_RO", strings.ToUpper(sha), 1, "ro").Val())
		require.Regexp(t, `^NOSCRIPT`, rdb.Do(ctx, "EVALSHA_RO", "ffd632c7d33e571e9f24556ebed26c3479a87130", 0).Err().Error())

		// no-writes allows reads and rejects writes
		require.Equal(t, "v", rdb.Do(ctx, "EVAL", "#!lua flags=no-writes\nreturn redis.call('get', 'ro')", 0).Val())
		require.Regexp(t, `^ERR Write commands are not allowed from read-only scripts\. script: [0-9a-f]{40}, on @user_script:2\.$`,
			evalErr(ctx, rdb, "#!lua flags=no-writes\nreturn redis.call('del', 'ro')"))
		require.Equal(t, "v", rdb.Get(ctx, "ro").Val())
		// and so does EVAL_RO, for every script
		require.Regexp(t, `^ERR Write commands are not allowed from read-only scripts\.`,
			rdb.Do(ctx, "EVAL_RO", `return redis.call('del', 'ro')`, 0).Err().Error())
		// a shebang script without no-writes is a write script: refused up front under *_ro
		require.EqualError(t, rdb.Do(ctx, "EVAL_RO", "#!lua\nreturn 1", 0).Err(),
			"ERR Can not execute a script with write flag using *_ro command.")
		// ... but runs under EVAL even if it only reads
		require.Equal(t, int64(1), rdb.Do(ctx, "EVAL", "#!lua\nreturn 1", 0).Val())

		// accepted flags (no-cluster is meaningless on a standalone server but valid)
		for _, flags := range []string{"allow-oom", "allow-stale", "no-cluster", "allow-cross-slot-keys", "no-writes,allow-stale", ""} {
			body := fmt.Sprintf("#!lua flags=%s\nreturn 'fine'", flags)
			require.Equal(t, "fine", rdb.Do(ctx, "EVAL", body, 0).Val(), flags)
		}
		// no-writes plus no-cluster, standalone half of the Kvrocks cluster tests
		require.Equal(t, "v", rdb.Do(ctx, "EVAL", "#!lua flags=no-writes,no-cluster\nreturn redis.call('get','ro')", 0).Val())
		require.Contains(t, evalErr(ctx, rdb, "#!lua flags=no-writes,no-cluster\nreturn redis.call('set','ro','x')"),
			"Write commands are not allowed from read-only scripts")
		// a shebang with no newline is invalid
		require.EqualError(t, rdb.Do(ctx, "EVAL", "#!lua", 0).Err(), "ERR Invalid script shebang")
	})

	t.Run("Arguments are binary safe and large scripts and arguments work", func(t *testing.T) {
		bin := "a\x00b\r\nc"
		require.Equal(t, bin, rdb.Eval(ctx, `return ARGV[1]`, nil, bin).Val())
		require.Equal(t, bin, rdb.Eval(ctx, `redis.call('set', KEYS[1], ARGV[1]) return redis.call('get', KEYS[1])`, []string{"binkey"}, bin).Val())
		big := strings.Repeat("x", 1<<20)
		require.Equal(t, int64(len(big)), rdb.Eval(ctx, `return #ARGV[1]`, nil, big).Val())
		bigScript := "local s = '" + strings.Repeat("y", 1<<20) + "' return #s"
		require.Equal(t, int64(1<<20), rdb.Eval(ctx, bigScript, nil).Val())
		// many arguments to redis.call
		require.Equal(t, int64(7999), rdb.Eval(ctx, `return #{unpack(ARGV)}`, nil, make7999()...).Val())
		require.Equal(t, int64(7999), rdb.Eval(ctx, `return redis.call('rpush', 'manyargs', unpack(ARGV))`, nil, make7999()...).Val())
	})
}

func make7999() []interface{} {
	out := make([]interface{}, 7999)
	for i := range out {
		out[i] = "v"
	}
	return out
}
