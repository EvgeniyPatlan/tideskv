// Package controller drives a TidesKV cluster with apache/kvrocks-controller 1.3.0 (M7a D26): topology
// pushes (SETNODEID + SETNODES), the controller's CLUSTER INFO / CLUSTER NODES parsers, and both failovers.
// scripts/docker-ci.sh starts the controller (etcd storage) and sets TIDESKV_CONTROLLER_ADDR.
package controller

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/stretchr/testify/require"

	"tideskv.local/compat/util"
)

type apiResponse struct {
	Data  json.RawMessage `json:"data"`
	Error *struct {
		Message string `json:"message"`
	} `json:"error"`
}

type ctlNode struct {
	ID   string `json:"id"`
	Addr string `json:"addr"`
	Role string `json:"role"`
}

type ctlShard struct {
	Nodes      []ctlNode `json:"nodes"`
	SlotRanges []string  `json:"slot_ranges"`
}

type ctlCluster struct {
	Version int64      `json:"version"`
	Shards  []ctlShard `json:"shards"`
}

type controller struct {
	t    *testing.T
	base string
	http *http.Client
}

func (c *controller) do(method, path string, body any) (int, apiResponse) {
	var reader io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		require.NoError(c.t, err)
		reader = bytes.NewReader(b)
	}
	req, err := http.NewRequest(method, c.base+path, reader)
	require.NoError(c.t, err)
	req.Header.Set("Content-Type", "application/json")
	resp, err := c.http.Do(req)
	if err != nil {
		return 0, apiResponse{}
	}
	defer resp.Body.Close()
	var out apiResponse
	_ = json.NewDecoder(resp.Body).Decode(&out)
	return resp.StatusCode, out
}

func (c *controller) cluster(ns string) ctlCluster {
	code, r := c.do(http.MethodGet, "/api/v1/namespaces/"+ns+"/clusters/c", nil)
	require.Equal(c.t, http.StatusOK, code, "%+v", r.Error)
	var w struct {
		Cluster ctlCluster `json:"cluster"`
	}
	require.NoError(c.t, json.Unmarshal(r.Data, &w))
	return w.Cluster
}

func masterOf(s ctlShard) ctlNode {
	for _, n := range s.Nodes {
		if n.Role == "master" {
			return n
		}
	}
	return ctlNode{}
}

// ownsSlot: slot_ranges entries are "N" or "a-b" (C:store/slot.go:91-96).
func ownsSlot(ranges []string, slot int) bool {
	for _, r := range ranges {
		var a, b int
		if n, _ := fmt.Sscanf(r, "%d-%d", &a, &b); n == 2 {
			if slot >= a && slot <= b {
				return true
			}
		} else if n, _ := fmt.Sscanf(r, "%d", &a); n == 1 && a == slot {
			return true
		}
	}
	return false
}

// nodesString renders the controller's view of a cluster as a CLUSTERX SETNODES argument (one
// "<id> <host> <port> <role> <master-id|-> [ranges]" line per node).
func nodesString(t *testing.T, c ctlCluster) string {
	var b strings.Builder
	for _, s := range c.Shards {
		master := masterOf(s)
		for _, n := range s.Nodes {
			host, port, err := net.SplitHostPort(n.Addr)
			require.NoError(t, err)
			if n.Role == "master" {
				fmt.Fprintf(&b, "%s %s %s master - %s\n", n.ID, host, port, strings.Join(s.SlotRanges, " "))
			} else {
				fmt.Fprintf(&b, "%s %s %s slave %s\n", n.ID, host, port, master.ID)
			}
		}
	}
	return b.String()
}

func TestControllerDrivesATidesKVCluster(t *testing.T) {
	addr := os.Getenv("TIDESKV_CONTROLLER_ADDR")
	if addr == "" {
		if os.Getenv("TIDESKV_CONTROLLER_REQUIRED") == "1" {
			t.Fatal("TIDESKV_CONTROLLER_REQUIRED=1 but TIDESKV_CONTROLLER_ADDR is not set")
		}
		t.Skip("tideskv: no kvrocks-controller (set TIDESKV_CONTROLLER_ADDR; scripts/docker-ci.sh compat starts one)")
	}
	host := os.Getenv("TIDESKV_CONTROLLER_NODE_HOST")
	if host == "" {
		host = "127.0.0.1"
	}
	ctx := context.Background()
	ctl := &controller{t: t, base: "http://" + addr, http: &http.Client{Timeout: 10 * time.Second}}
	ns := fmt.Sprintf("tideskv-%d", time.Now().UnixNano())
	require.Eventually(t, func() bool { // the controller elects itself leader through etcd first
		code, _ := ctl.do(http.MethodPost, "/api/v1/namespaces", map[string]string{"namespace": ns})
		return code == http.StatusCreated || code == http.StatusConflict
	}, 60*time.Second, 500*time.Millisecond)
	defer ctl.do(http.MethodDelete, "/api/v1/namespaces/"+ns, nil)

	servers := make([]*util.KvrocksServer, 4)
	clients := make([]*redis.Client, 4)
	addrs := make([]string, 4)
	closed := make([]bool, 4)
	for i := range servers {
		servers[i] = util.StartServer(t, map[string]string{"cluster-enabled": "yes", "bind": "0.0.0.0"})
		clients[i] = servers[i].NewClient()
		addrs[i] = fmt.Sprintf("%s:%d", host, servers[i].Port())
	}
	clientAt := func(addr string) *redis.Client {
		for i := range addrs {
			if addrs[i] == addr {
				return clients[i]
			}
		}
		t.Fatalf("no node at %s", addr)
		return nil
	}
	defer func() {
		for i := range servers {
			_ = clients[i].Close()
			if !closed[i] {
				servers[i].Close()
			}
		}
	}()

	// Shard 0 = nodes 0 (master), 1; shard 1 = nodes 2 (master), 3; version 1 (C:store/cluster.go:42-78).
	code, r := ctl.do(http.MethodPost, "/api/v1/namespaces/"+ns+"/clusters",
		map[string]any{"name": "c", "nodes": addrs, "replicas": 2})
	require.Equal(t, http.StatusCreated, code, "%+v", r.Error)
	defer ctl.do(http.MethodDelete, "/api/v1/namespaces/"+ns+"/clusters/c", nil)

	t.Run("the topology reaches every node on the next probe", func(t *testing.T) {
		want := strconv.FormatInt(ctl.cluster(ns).Version, 10)
		for i, c := range clients {
			require.Eventually(t, func() bool { return c.Do(ctx, "CLUSTERX", "VERSION").Val() == want },
				30*time.Second, 200*time.Millisecond, "node %d", i)
		}
		require.Equal(t, "slave", util.FindInfoEntry(clients[1], "role"))
		require.Equal(t, "slave", util.FindInfoEntry(clients[3], "role"))
		util.WaitForSync(t, clients[1])
		util.WaitForSync(t, clients[3])
	})

	t.Run("a node ahead of the store is adopted through CLUSTER INFO", func(t *testing.T) {
		// The controller reads cluster_current_epoch with a CRLF line split; a node whose epoch is above the
		// store's makes it pull that node's CLUSTER NODES into the store and push the new version to the
		// others (C:controller/cluster.go:242-287). A misparsed epoch (-1) would re-push the older version.
		cur := ctl.cluster(ns)
		next := strconv.FormatInt(cur.Version+1, 10)
		require.NoError(t, clients[0].Do(ctx, "CLUSTERX", "SETNODES", nodesString(t, cur), next).Err())
		require.Eventually(t, func() bool { return ctl.cluster(ns).Version == cur.Version+1 },
			30*time.Second, 200*time.Millisecond)
		for i, c := range clients {
			require.Eventually(t, func() bool { return c.Do(ctx, "CLUSTERX", "VERSION").Val() == next },
				30*time.Second, 200*time.Millisecond, "node %d", i)
		}
	})

	t.Run("CLUSTER NODES passes the controller's ParseCluster", func(t *testing.T) {
		// Import parses node 0's CLUSTER NODES first and only then finds the addresses already in use: 409
		// proves the parse succeeded (C:server/api/cluster.go:164-205).
		code, r := ctl.do(http.MethodPost, "/api/v1/namespaces/"+ns+"/clusters/imported/import",
			map[string]any{"nodes": []string{addrs[0]}})
		require.Equal(t, http.StatusConflict, code, "%+v", r.Error)
	})

	cc := redis.NewClusterClient(&redis.ClusterOptions{Addrs: addrs})
	defer cc.Close()
	t.Run("a cluster client writes through both shards", func(t *testing.T) {
		for i := 0; i < 200; i++ {
			require.NoError(t, cc.Set(ctx, fmt.Sprintf("key:%d", i), i, 0).Err())
		}
		util.WaitForOffsetSync(t, clients[0], clients[1], 10*time.Second)
		util.WaitForOffsetSync(t, clients[2], clients[3], 10*time.Second)
	})

	t.Run("a manual failover promotes the replica", func(t *testing.T) {
		code, r := ctl.do(http.MethodPost, "/api/v1/namespaces/"+ns+"/clusters/c/shards/0/failover", map[string]string{})
		require.Equal(t, http.StatusOK, code, "%+v", r.Error)
		require.Eventually(t, func() bool {
			return util.FindInfoEntry(clients[1], "role") == "master" && util.FindInfoEntry(clients[0], "role") == "slave"
		}, 30*time.Second, 200*time.Millisecond)
		require.Equal(t, masterOf(ctl.cluster(ns).Shards[0]).ID, clients[1].Do(ctx, "CLUSTERX", "MYID").Val())
		util.WaitForSync(t, clients[0])
		cc.ReloadState(ctx)
		for i := 0; i < 200; i++ {
			require.Equal(t, strconv.Itoa(i), cc.Get(ctx, fmt.Sprintf("key:%d", i)).Val())
		}
	})

	t.Run("a migration moves slot 100 with its keys and cleans the source", func(t *testing.T) { // M7b D1, D7
		keys := make([]string, 50)
		for i := range keys {
			keys[i] = fmt.Sprintf("{%s}:%d", util.SlotTable[100], i)
			require.NoError(t, cc.Set(ctx, keys[i], i, 0).Err())
		}
		source := clientAt(masterOf(ctl.cluster(ns).Shards[0]).Addr)
		code, r := ctl.do(http.MethodPost, "/api/v1/namespaces/"+ns+"/clusters/c/migrate",
			map[string]any{"target": 1, "slot": "100", "slot_only": false})
		require.Equal(t, http.StatusOK, code, "%+v", r.Error)
		require.Eventually(t, func() bool { // the controller polls the source's CLUSTER INFO every second
			c := ctl.cluster(ns)
			return ownsSlot(c.Shards[1].SlotRanges, 100) && !ownsSlot(c.Shards[0].SlotRanges, 100)
		}, 60*time.Second, 500*time.Millisecond)
		cc.ReloadState(ctx)
		for i, k := range keys {
			require.Equal(t, strconv.Itoa(i), cc.Get(ctx, k).Val(), k)
		}
		require.Eventually(t, func() bool {
			return source.Do(ctx, "CLUSTER", "COUNTKEYSINSLOT", 100).Val() == int64(0)
		}, 30*time.Second, 200*time.Millisecond)
	})

	t.Run("a slot_only move reassigns slot 101 and leaves its keys", func(t *testing.T) { // M7b D7, KM §4.2
		key := fmt.Sprintf("{%s}:kept", util.SlotTable[101])
		require.NoError(t, cc.Set(ctx, key, "v", 0).Err())
		source := clientAt(masterOf(ctl.cluster(ns).Shards[0]).Addr)
		code, r := ctl.do(http.MethodPost, "/api/v1/namespaces/"+ns+"/clusters/c/migrate",
			map[string]any{"target": 1, "slot": "101", "slot_only": true})
		require.Equal(t, http.StatusOK, code, "%+v", r.Error)
		want := strconv.FormatInt(ctl.cluster(ns).Version, 10)
		require.True(t, ownsSlot(ctl.cluster(ns).Shards[1].SlotRanges, 101))
		require.Eventually(t, func() bool { return source.Do(ctx, "CLUSTERX", "VERSION").Val() == want },
			30*time.Second, 200*time.Millisecond)
		// tideskv: a slot lost without a migration keeps its data, as Kvrocks; CLUSTERX FLUSHSLOTS removes it
		require.EqualValues(t, 1, source.Do(ctx, "CLUSTER", "COUNTKEYSINSLOT", 101).Val())
		cc.ReloadState(ctx)
		require.ErrorIs(t, cc.Get(ctx, key).Err(), redis.Nil)
	})

	t.Run("a dead master is failed over automatically", func(t *testing.T) {
		servers[2].Close()
		closed[2] = true
		require.Eventually(t, func() bool { return util.FindInfoEntry(clients[3], "role") == "master" },
			60*time.Second, 500*time.Millisecond)
		require.Equal(t, addrs[3], masterOf(ctl.cluster(ns).Shards[1]).Addr)
	})
}
