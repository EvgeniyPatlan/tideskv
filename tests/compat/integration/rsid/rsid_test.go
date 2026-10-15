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

// Ported to TidesKV from apache/kvrocks v2.17.0 tests/gocase/integration/rsid/rsid_test.go; changes are marked "tideskv:".

package rsid

import (
	"context"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
	"tideskv.local/compat/util" // tideskv: was github.com/apache/kvrocks/tests/gocase/util
)

func TestRSIDMasterAndReplicaYes(t *testing.T) {
	ctx := context.Background()

	srvA := util.StartServer(t, map[string]string{}) // tideskv: one protocol, no use-rsid-psync
	defer func() { srvA.Close() }()
	rdbA := srvA.NewClient()
	defer func() { require.NoError(t, rdbA.Close()) }()
	require.NoError(t, rdbA.Set(ctx, "a", "b", 0).Err())

	srvB := util.StartServer(t, map[string]string{})
	defer func() { srvB.Close() }()
	rdbB := srvB.NewClient()
	defer func() { require.NoError(t, rdbB.Close()) }()
	require.NoError(t, rdbB.Set(ctx, "c", "d", 0).Err())

	t.Run("replica psync sequence is in the range the master wal but full sync", func(t *testing.T) {
		require.Equal(t, util.FindInfoEntry(rdbA, "master_repl_offset"), util.FindInfoEntry(rdbB, "master_repl_offset"))
		util.SlaveOf(t, rdbA, srvB)
		util.WaitForSync(t, rdbA)
		require.Equal(t, "1", util.FindInfoEntry(rdbB, "sync_full"))
		// tideskv: the stream continues on the full-sync link; Kvrocks reconnects with a PSYNC
		require.Equal(t, "0", util.FindInfoEntry(rdbB, "sync_partial_ok"))
		require.Equal(t, "", rdbA.Get(ctx, "a").Val())
		require.Equal(t, "d", rdbA.Get(ctx, "c").Val())
	})
	// tideskv: the chained-replication subtests need a replica that serves PSYNC; TidesKV has none (M6a F8)
}

// tideskv: not in Kvrocks. An old master that wrote after the split is fully resynced by the new one.
func TestDivergentOldMasterIsFullySynced(t *testing.T) {
	ctx := context.Background()
	srvA := util.StartServer(t, map[string]string{})
	defer func() { srvA.Close() }()
	rdbA := srvA.NewClient()
	defer func() { require.NoError(t, rdbA.Close()) }()
	srvB := util.StartServer(t, map[string]string{})
	defer func() { srvB.Close() }()
	rdbB := srvB.NewClient()
	defer func() { require.NoError(t, rdbB.Close()) }()
	require.NoError(t, rdbA.Set(ctx, "base", "1", 0).Err())
	util.SlaveOf(t, rdbB, srvA)
	util.WaitForSync(t, rdbB)
	util.WaitForOffsetSync(t, rdbA, rdbB, 5*time.Second)
	require.NoError(t, rdbB.SlaveOf(ctx, "NO", "ONE").Err())
	require.NoError(t, rdbA.Set(ctx, "only-on-a", "1", 0).Err())
	require.NoError(t, rdbB.Set(ctx, "only-on-b", "1", 0).Err())
	before := util.FindInfoEntry(rdbB, "sync_full")
	util.SlaveOf(t, rdbA, srvB)
	util.WaitForSync(t, rdbA)
	require.NotEqual(t, before, util.FindInfoEntry(rdbB, "sync_full"))
	util.WaitForOffsetSync(t, rdbB, rdbA, 10*time.Second)
	require.EqualValues(t, 0, rdbA.Exists(ctx, "only-on-a").Val())
	require.EqualValues(t, 1, rdbA.Exists(ctx, "only-on-b").Val())
	require.Equal(t, rdbB.Do(ctx, "DEBUG", "DIGEST").Val(), rdbA.Do(ctx, "DEBUG", "DIGEST").Val())
}
