/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Ported to TidesKV from apache/kvrocks v2.17.0 tests/gocase/unit/scripting/scripting_test.go; changes are marked "tideskv:".
// tideskv: the cluster subtests of TestEvalScriptFlags (no-cluster, allow-cross-slot-keys, mixed use) live here,
// in TestEvalScriptFlagsCluster with its own standalone server, and with them the declared-key stub
// TestEvalScriptInStrictMode, so scripting_test.go stays under 800 lines.

package scripting

import (
	"context"
	"fmt"
	"testing"

	"github.com/stretchr/testify/require"

	"tideskv.local/compat/util" // tideskv: was github.com/apache/kvrocks/tests/gocase/util
)

func TestEvalScriptFlagsCluster(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	// tideskv: Redis 7.2 errors carry no "<sha>: " prefix before the cluster-script messages, so the "ERR .* " regexps become "ERR " (M7a D15)
	srv0 := util.StartServer(t, map[string]string{"cluster-enabled": "yes"})
	rdb0 := srv0.NewClient()
	defer func() { require.NoError(t, rdb0.Close()) }()
	defer func() { srv0.Close() }()
	id0 := "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx00"
	require.NoError(t, rdb0.Do(ctx, "clusterx", "SETNODEID", id0).Err())

	srv1 := util.StartServer(t, map[string]string{"cluster-enabled": "yes"})
	srv1Alive := true
	defer func() {
		if srv1Alive {
			srv1.Close()
		}
	}()

	rdb1 := srv1.NewClient()
	defer func() { require.NoError(t, rdb1.Close()) }()
	id1 := "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx01"
	require.NoError(t, rdb1.Do(ctx, "clusterx", "SETNODEID", id1).Err())

	clusterNodes := fmt.Sprintf("%s %s %d master - 0-10000\n", id0, srv0.Host(), srv0.Port())
	clusterNodes += fmt.Sprintf("%s %s %d master - 10001-16383", id1, srv1.Host(), srv1.Port())
	require.NoError(t, rdb0.Do(ctx, "clusterx", "SETNODES", clusterNodes, "1").Err())
	require.NoError(t, rdb1.Do(ctx, "clusterx", "SETNODES", clusterNodes, "1").Err())

	t.Run("no-cluster", func(t *testing.T) {
		r := rdb0.Do(ctx, "EVAL",
			`#!lua flags=no-cluster
		return redis.call('set', 'k','v');`, "0")
		util.ErrorRegexp(t, r.Err(), "ERR Can not run script on cluster, 'no-cluster' flag is set")

		// Only valid in cluster mode
		r = rdb.Do(ctx, "EVAL",
			`#!lua flags=no-cluster
		return redis.call('set', 'k','v');`, "0")
		require.NoError(t, r.Err())

		// Scripts without #! can run commands that access keys belonging to different cluster hash slots,
		// but ones with #! inherit the default flags, so they cannot.
		r = rdb0.Do(ctx, "EVAL", `return redis.call('set', 'k','v');`, "0")
		require.NoError(t, r.Err())

		r = rdb0.Do(ctx, "EVAL",
			`#!lua
		return redis.call('set', 'k','v');`, "0")
		require.NoError(t, r.Err())
	})

	t.Run("allow-cross-slot-keys", func(t *testing.T) {
		// Node0: bar-slot = 5061, test-slot = 6918
		// Node1: foo-slot = 12182
		// Different slots of different nodes are not affected by allow-cross-slot-keys,
		// and different slots of the same node can be allowed
		r := rdb0.Do(ctx, "EVAL",
			`#!lua flags=allow-cross-slot-keys
		redis.call('set', 'bar','value_bar');
		return redis.call('set', 'test', 'value_test');`, "0")
		require.NoError(t, r.Err())

		r = rdb0.Do(ctx, "EVAL",
			`#!lua flags=allow-cross-slot-keys
		redis.call('set', 'foo','value_foo');
		return redis.call('set', 'bar', 'value_bar');`, "0")
		util.ErrorRegexp(t, r.Err(), "ERR Script attempted to access a non local key in a cluster node script")

		// There is a shebang prefix #!lua but crossslot is not allowed when flags are not set
		r = rdb0.Do(ctx, "EVAL",
			`#!lua
		redis.call('get', 'bar');
		return redis.call('get', 'test');`, "0")
		util.ErrorRegexp(t, r.Err(), "ERR Script attempted to access keys that do not hash to the same slot")

		r = rdb0.Do(ctx, "EVAL",
			`#!lua
		redis.call('get', 'foo');
		return redis.call('get', 'bar');`, "0")
		util.ErrorRegexp(t, r.Err(), "ERR Script attempted to access a non local key in a cluster node script")

		// Old style: CrossSlot is allowed when there is neither #!lua nor flags set
		r = rdb0.Do(ctx, "EVAL",
			`redis.call('get', 'bar');
		return redis.call('get', 'test');`, "0")
		require.NoError(t, r.Err())

		r = rdb0.Do(ctx, "EVAL",
			`redis.call('get', 'foo');
		return redis.call('get', 'bar');`, "0")
		util.ErrorRegexp(t, r.Err(), "ERR Script attempted to access a non local key in a cluster node script")

		// Pre-declared keys are not affected by allow-cross-slot-keys
		r = rdb0.Do(ctx, "EVAL",
			`#!lua flags=allow-cross-slot-keys
		local key = redis.call('get', KEY[1]);
		return redis.call('get', KEY[2]);`, "2", "bar", "test")
		// tideskv: Redis 7.2 routes the declared KEYS at EVAL and answers its generic CROSSSLOT text (M7a D14/D15)
		require.EqualError(t, r.Err(), "CROSSSLOT Keys in request don't hash to the same slot")
	})

	t.Run("mixed use", func(t *testing.T) {
		r := rdb0.Do(ctx, "EVAL",
			`#!lua flags=no-writes,no-cluster
		return redis.call('get', 'key_a');`, "0")
		util.ErrorRegexp(t, r.Err(), "ERR Can not run script on cluster, 'no-cluster' flag is set")

		r = rdb.Do(ctx, "EVAL",
			`#!lua flags=no-writes,no-cluster
		return redis.call('set', 'key_a', 'value_a');`, "0")
		util.ErrorRegexp(t, r.Err(), "ERR Write commands are not allowed from read-only scripts")

		err := rdb.Set(ctx, "key_a", "value_a", 0).Err()
		require.NoError(t, err)
		r = rdb.Do(ctx, "EVAL",
			`#!lua flags=no-writes,no-cluster
		return redis.call('get', 'key_a');`, "0")
		require.NoError(t, r.Err())

		r = rdb0.Do(ctx, "EVAL",
			`#!lua flags=no-writes,allow-cross-slot-keys
		redis.call('get', 'bar');
		return redis.call('get', 'test');`, "0")
		require.NoError(t, r.Err())

		r = rdb0.Do(ctx, "EVAL",
			`#!lua flags=no-writes,allow-cross-slot-keys
		redis.call('set', 'bar', 'value');
		return redis.call('set', 'test', 'value');`, "0")
		util.ErrorRegexp(t, r.Err(), "ERR Write commands are not allowed from read-only scripts")

		r = rdb0.Do(ctx, "EVAL",
			`#!lua flags=no-writes,allow-cross-slot-keys
		redis.call('get', 'bar');
		return redis.call('get', 'foo');`, "0")
		util.ErrorRegexp(t, r.Err(), "ERR Script attempted to access a non local key in a cluster node script")

	})
}

func TestEvalScriptInStrictMode(t *testing.T) {
	t.Run("Accessing undeclared keys in strict mode", func(t *testing.T) {
		t.Skip("tideskv: lua-strict-key-accessing is a Kvrocks-only config with Kvrocks-only semantics")
	})
}
