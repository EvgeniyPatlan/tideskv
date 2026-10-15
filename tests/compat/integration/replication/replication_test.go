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

// Ported to TidesKV from apache/kvrocks v2.17.0 tests/gocase/integration/replication/replication_test.go; changes are marked "tideskv:".

package replication

import (
	"context"
	"fmt"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/stretchr/testify/require"
	"tideskv.local/compat/util" // tideskv: was github.com/apache/kvrocks/tests/gocase/util
)

func TestClusterReplication(t *testing.T) {
	t.Skip("tideskv: CLUSTERX is M7")
}

func TestReplicationWithHostname(t *testing.T) {
	t.Parallel()
	srvA := util.StartServer(t, map[string]string{})
	defer srvA.Close()
	rdbA := srvA.NewClient()
	defer func() { require.NoError(t, rdbA.Close()) }()
	util.Populate(t, rdbA, "", 100, 10)

	srvB := util.StartServer(t, map[string]string{})
	defer srvB.Close()
	rdbB := srvB.NewClient()
	defer func() { require.NoError(t, rdbB.Close()) }()

	t.Run("Set instance A as slave of B with localhost, for issue #1182", func(t *testing.T) {
		require.NoError(t, rdbA.SlaveOf(context.Background(), "localhost", fmt.Sprintf("%d", srvB.Port())).Err())
		util.SlaveOf(t, rdbA, srvB)
		util.WaitForSync(t, rdbA)
		ctx := context.Background()

		require.NoError(t, rdbB.Set(ctx, "mykey", "foo", 0).Err())
		require.Eventually(t, func() bool {
			return rdbA.Get(ctx, "mykey").Val() == "foo"
		}, 50*time.Second, 100*time.Millisecond)
	})
}

func TestReplicationTransferCommandsRequireAdminPermission(t *testing.T) {
	t.Skip("tideskv: no namespaces/ACL")
}

func TestReplicationLoading(t *testing.T) {
	t.Parallel()
	srvA := util.StartServer(t, map[string]string{})
	defer srvA.Close()
	rdbA := srvA.NewClient()
	defer func() { require.NoError(t, rdbA.Close()) }()
	util.Populate(t, rdbA, "", 100, 10)

	srvB := util.StartServer(t, map[string]string{})
	defer srvB.Close()
	rdbB := srvB.NewClient()
	defer func() { require.NoError(t, rdbB.Close()) }()
	util.Populate(t, rdbB, "b", 5000, 10) // tideskv: something to load slowly

	t.Run("Set instance A as slave of B", func(t *testing.T) {
		ctx := context.Background()
		// tideskv: Redis's hidden key-load-delay (µs per key) replaces fullsync-recv-file-delay
		require.NoError(t, rdbA.ConfigSet(ctx, "key-load-delay", "400").Err())
		util.SlaveOf(t, rdbA, srvB)
		require.Eventually(t, func() bool {
			return util.FindInfoEntry(rdbA, "loading") == "1"
		}, 5*time.Second, 50*time.Millisecond)
		require.Eventually(t, func() bool {
			return util.FindInfoEntry(rdbA, "loading") == "0"
		}, 50*time.Second, 100*time.Millisecond)
		require.NoError(t, rdbA.ConfigSet(ctx, "key-load-delay", "0").Err())
		util.WaitForSync(t, rdbA)
	})
}

func TestReplicationBasics(t *testing.T) {
	t.Parallel()
	master := util.StartServer(t, map[string]string{})
	defer master.Close()
	masterClient := master.NewClient()
	defer func() { require.NoError(t, masterClient.Close()) }()

	ctx := context.Background()
	require.NoError(t, masterClient.Set(ctx, "mykey", "foo", 0).Err())
	require.NoError(t, masterClient.Set(ctx, "mystring", "a", 0).Err())
	require.NoError(t, masterClient.LPush(ctx, "mylist", "a", "b", "c").Err())
	require.NoError(t, masterClient.SAdd(ctx, "myset", "a", "b", "c").Err())
	require.NoError(t, masterClient.HMSet(ctx, "myhash", "a", 1, "b", 2, "c", 3).Err())
	require.NoError(t, masterClient.ZAdd(ctx, "myzset",
		redis.Z{Score: 1, Member: "a"},
		redis.Z{Score: 2, Member: "b"},
		redis.Z{Score: 3, Member: "c"},
	).Err())
	// tideskv: the cuckoo-filter (cf.*) commands are Kvrocks-only and removed

	slave := util.StartServer(t, map[string]string{})
	defer slave.Close()
	slaveClient := slave.NewClient()
	defer func() { require.NoError(t, slaveClient.Close()) }()

	t.Run("Second server should have role master at first", func(t *testing.T) {
		util.Populate(t, slaveClient, "", 100, 10)
		require.Equal(t, "master", util.FindInfoEntry(slaveClient, "role"))
	})

	t.Run("The role should immediately be changed to 'replica'", func(t *testing.T) {
		util.SlaveOf(t, slaveClient, master)
		require.Equal(t, "slave", util.FindInfoEntry(slaveClient, "role"))
	})
	util.WaitForSync(t, slaveClient)
	t.Run("Sync should have transferred keys from master", func(t *testing.T) {
		require.Equal(t, masterClient.Get(ctx, "mykey"), slaveClient.Get(ctx, "mykey"))
		require.Equal(t, masterClient.Get(ctx, "mystring"), slaveClient.Get(ctx, "mystring"))
		require.Equal(t, masterClient.LRange(ctx, "mylist", 0, -1),
			slaveClient.LRange(ctx, "mylist", 0, -1))
		require.Equal(t, masterClient.HGetAll(ctx, "myhash"), slaveClient.HGetAll(ctx, "myhash"))
		require.Equal(t, masterClient.ZRangeWithScores(ctx, "myzset", 0, -1),
			slaveClient.ZRangeWithScores(ctx, "myzset", 0, -1))
		require.Equal(t, masterClient.SMembers(ctx, "myhash"), slaveClient.SMembers(ctx, "myhash"))
	})

	t.Run("The link status should be up", func(t *testing.T) {
		require.Equal(t, "up", util.FindInfoEntry(slaveClient, "master_link_status"))
	})

	t.Run("SET on the master should immediately propagate", func(t *testing.T) {
		require.NoError(t, masterClient.Set(ctx, "mykey", "bar", 0).Err())
		require.Eventually(t, func() bool {
			return slaveClient.Get(ctx, "mykey").Val() == "bar"
		}, 50*time.Second, 100*time.Millisecond)
	})

	t.Run("FLUSHALL should be replicated", func(t *testing.T) {
		require.NoError(t, masterClient.FlushAll(ctx).Err())
		time.Sleep(100 * time.Millisecond)
		// tideskv: no DBSIZE SCAN (Kvrocks-only)
		require.Eventually(t, func() bool {
			masterDBSize, err := masterClient.Do(ctx, "dbsize").Result()
			require.NoError(t, err)
			slaveDBSize, err := slaveClient.Do(ctx, "dbsize").Result()
			require.NoError(t, err)
			return masterDBSize.(int64) == 0 && slaveDBSize.(int64) == 0
		}, 5*time.Second, 100*time.Millisecond)
	})

	t.Run("ROLE in master reports master with a slave", func(t *testing.T) {
		vals, err := masterClient.Do(ctx, "role").Slice()
		require.NoError(t, err)
		require.EqualValues(t, 3, len(vals))
		// The order of vals in master is: role, offset, slaves
		require.EqualValues(t, "master", vals[0])
		masterOffset, ok := vals[1].(int64) // tideskv: Redis 7.2 ROLE offset is an integer (Kvrocks: string)
		require.True(t, ok)
		require.Greater(t, masterOffset, int64(0))
		slaves, ok := vals[2].([]interface{})
		require.True(t, ok)
		require.EqualValues(t, 1, len(slaves))
		slave0, _ := slaves[0].([]interface{})
		require.EqualValues(t, 3, len(slave0))
		slaveOffset, err := strconv.ParseInt(slave0[2].(string), 10, 64)
		require.NoError(t, err)
		util.BetweenValues(t, slaveOffset, 1, masterOffset)
	})

	t.Run("ROLE in slave reports slave in connected state", func(t *testing.T) {
		vals, err := slaveClient.Do(ctx, "role").Slice() // tideskv: master_port and offset are integers in Redis 7.2
		require.NoError(t, err)
		require.EqualValues(t, 5, len(vals))
		// The order of vals in slave is: role, master_host, master_port, slave_state, offset
		require.Equal(t, "slave", vals[0])
		require.Equal(t, "connected", vals[3])
	})
}

func TestReplicationWithMultiSlaves(t *testing.T) {
	t.Parallel()
	srvA := util.StartServer(t, map[string]string{})
	defer srvA.Close()
	rdbA := srvA.NewClient()
	defer func() { require.NoError(t, rdbA.Close()) }()
	util.Populate(t, rdbA, "", 100, 10)

	srvB := util.StartServer(t, map[string]string{})
	defer srvB.Close()
	rdbB := srvB.NewClient()
	defer func() { require.NoError(t, rdbB.Close()) }()
	util.Populate(t, rdbB, "", 100, 10)

	srvC := util.StartServer(t, map[string]string{})
	defer srvC.Close()
	rdbC := srvC.NewClient()
	defer func() { require.NoError(t, rdbC.Close()) }()
	util.Populate(t, rdbC, "", 50, 10)

	t.Run("Multi slaves full sync with master at the same time", func(t *testing.T) {
		util.SlaveOf(t, rdbA, srvC)
		util.SlaveOf(t, rdbB, srvC)
		util.WaitForSync(t, rdbA)
		util.WaitForSync(t, rdbB)
		require.Eventually(t, func() bool {
			roleA := rdbA.Do(context.Background(), "role").String()
			roleB := rdbB.Do(context.Background(), "role").String()
			return strings.Contains(roleA, "connected") && strings.Contains(roleB, "connected")
		}, 50*time.Second, 100*time.Millisecond)
		require.Equal(t, "2", util.FindInfoEntry(rdbC, "sync_full"))
	})
}

func TestReplicationWithLimitSpeed(t *testing.T) {
	t.Skip("tideskv: no per-file transfer: full sync is a snapshot stream, M6b D1")
}

func TestReplicationShareCheckpoint(t *testing.T) {
	t.Skip("tideskv: no checkpoint: each replica gets its own snapshot, M6b D1")
}

func TestReplicationContinueRunning(t *testing.T) {
	t.Parallel()
	master := util.StartServer(t, map[string]string{})
	defer master.Close()
	masterClient := master.NewClient()
	defer func() { require.NoError(t, masterClient.Close()) }()

	slave := util.StartServer(t, map[string]string{})
	defer slave.Close()
	slaveClient := slave.NewClient()
	defer func() { require.NoError(t, slaveClient.Close()) }()

	util.SlaveOf(t, slaveClient, master)
	util.WaitForSync(t, slaveClient)

	t.Run("Master doesn't pause replicating with replicas, #346", func(t *testing.T) {
		ctx := context.Background()
		// In #346, we find a bug, if one command contains more than special
		// number updates, master won't send replication stream to replicas.
		masterClient.HSet(ctx, "myhash", map[string]interface{}{
			"0": 0, "1": 1, "2": 2, "3": 3, "4": 4, "5": 5, "6": 6, "7": 7, "8": 8, "9": 9,
			"a": "a", "b": "b", "c": "c", "d": "d", "e": "e", "f": "f", "g": "g", "h": "h", "i": "i", "j": "j", "k": "k"})
		require.EqualValues(t, 21, masterClient.HLen(ctx, "myhash").Val())
		util.WaitForOffsetSync(t, masterClient, slaveClient, 5*time.Second)
		require.Equal(t, "1", slaveClient.HGet(ctx, "myhash", "1").Val())
		require.Equal(t, "a", slaveClient.HGet(ctx, "myhash", "a").Val())
	})
}

func TestReplicationChangePassword(t *testing.T) {
	t.Skip("tideskv: CLIENT KILL TYPE slave is not implemented")
}

func TestReplicationAnnounceIP(t *testing.T) {
	t.Parallel()
	master := util.StartServer(t, map[string]string{})
	defer master.Close()
	masterClient := master.NewClient()
	defer func() { require.NoError(t, masterClient.Close()) }()

	ctx := context.Background()

	slave := util.StartServer(t, map[string]string{"replica-announce-ip": "slave-ip.local", "replica-announce-port": "1234"})
	defer slave.Close()
	slaveClient := slave.NewClient()
	defer func() { require.NoError(t, slaveClient.Close()) }()

	t.Run("Setup second server as replica", func(t *testing.T) {
		util.SlaveOf(t, slaveClient, master)
		require.Equal(t, "slave", util.FindInfoEntry(slaveClient, "role"))
	})

	util.WaitForSync(t, slaveClient)
	t.Run("INFO master for slave0 should contain replica-announce-ip and replica-announce-port", func(t *testing.T) {
		value := util.FindInfoEntry(masterClient, "slave0")
		require.Contains(t, value, "ip=slave-ip.local,port=1234")
	})

	t.Run("ROLE in master reports slaves replica-announce-ip and replica-announce-port", func(t *testing.T) {
		vals, err := masterClient.Do(ctx, "role").Slice()
		require.NoError(t, err)
		require.EqualValues(t, 3, len(vals))
		slaves, ok := vals[2].([]interface{})
		require.True(t, ok)
		slave0, ok := slaves[0].([]interface{})
		require.True(t, ok)
		require.EqualValues(t, 3, len(slave0))

		slave0ip, ok := slave0[0].(string)
		require.True(t, ok)

		slave0port, ok := slave0[1].(string)
		require.True(t, ok)

		require.Equal(t, "slave-ip.local", slave0ip)
		require.Equal(t, "1234", slave0port)
	})
}

func TestShouldNotReplicate(t *testing.T) {
	t.Skip("tideskv: Kvrocks-only checks; Redis 7.2 has no such errors")
}

func TestFullSyncReplication(t *testing.T) {
	t.Parallel()
	// tideskv: the backlog keeps only the newest batch, so the replica must full-sync
	master := util.StartServer(t, map[string]string{"repl-backlog-size": "1"})
	defer master.Close()
	masterClient := master.NewClient()
	defer func() { require.NoError(t, masterClient.Close()) }()

	slave := util.StartServer(t, map[string]string{})
	defer slave.Close()
	slaveClient := slave.NewClient()
	defer func() { require.NoError(t, slaveClient.Close()) }()

	ctx := context.Background()

	t.Run("Full sync replication should work correctly", func(t *testing.T) {
		// tideskv: the replica writes as a standalone node first, so it presents its own replid and is
		// full-synced deterministically instead of racing the backlog trimmer (preflight PF1/PF19)
		require.NoError(t, slaveClient.Set(ctx, "standalone", "1", 0).Err())

		value := strings.Repeat("a", 128*1024)
		for i := 0; i < 1024; i++ {
			require.NoError(t, masterClient.Set(ctx, fmt.Sprintf("key%d", i), value, 0).Err())
		}

		util.SlaveOf(t, slaveClient, master)
		// Wait more time for full sync to avoid flake test in CI environment
		util.WaitForOffsetSync(t, masterClient, slaveClient, 60*time.Second)

		// Make sure the full sync happened in replication
		syncFullCount, err := strconv.Atoi(util.FindInfoEntry(masterClient, "sync_full"))
		require.NoError(t, err)
		require.Greater(t, syncFullCount, 0)

		got, err := slaveClient.Get(ctx, "key1").Result()
		require.NoError(t, err)
		require.Equal(t, value, got)

		require.NoError(t, masterClient.Set(ctx, "foo", "bar", 0).Err())
		util.WaitForOffsetSync(t, masterClient, slaveClient, 5*time.Second)
		require.Equal(t, "bar", slaveClient.Get(ctx, "foo").Val())
	})
}

func TestSlaveLostMaster(t *testing.T) {
	t.Skip("tideskv: CLUSTERX SETNODES is M7; the standalone case is FullSyncIT.ReplicaofAnotherMasterDuringTheLoadAbortsIt")
}

func TestReplicationGroupSyncConfig(t *testing.T) {
	t.Skip("tideskv: RocksDB-specific")
}

func TestReplicationWatermark(t *testing.T) {
	t.Parallel()
	master := util.StartServer(t, map[string]string{})
	defer master.Close()
	masterClient := master.NewClient()
	defer func() { require.NoError(t, masterClient.Close()) }()

	slave := util.StartServer(t, map[string]string{})
	defer slave.Close()
	slaveClient := slave.NewClient()
	defer func() { require.NoError(t, slaveClient.Close()) }()

	ctx := context.Background()
	util.SlaveOf(t, slaveClient, master)
	util.WaitForSync(t, slaveClient)

	// Send a large SET command to trigger a high watermark in the slave
	largeValue := strings.Repeat("a", 16*1024) // 16KB value
	require.NoError(t, masterClient.Set(ctx, "large_key", largeValue, 0).Err())

	// Wait a bit for the large command to be processed
	time.Sleep(50 * time.Millisecond)

	// Immediately send a small SET command
	// Without the fix, this would be delayed due to the high watermark
	start := time.Now()
	require.NoError(t, masterClient.Set(ctx, "small_key", "small_value", 0).Err())

	// Check if the small SET is processed quickly on the slave
	// The small command should appear within 1 second (much faster than the buggy 1 minute delay)
	require.Eventually(t, func() bool {
		val, err := slaveClient.Get(ctx, "small_key").Result()
		if err != nil {
			return false
		}
		return val == "small_value"
	}, 1*time.Second, 50*time.Millisecond, "slave should process small command quickly after large command")

	duration := time.Since(start)
	// The small command should be processed much faster than 1 second
	require.Less(t, duration, 1*time.Second, "small command should be processed promptly")
}

func TestReplicationSlowConsumerConfig(t *testing.T) {
	t.Skip("tideskv: no slow-consumer knobs: senders keep no output buffer, M6b D15")
}

func TestReplicationExponentialBackoff(t *testing.T) {
	t.Skip("tideskv: CLIENT KILL TYPE slave is not implemented")
}
