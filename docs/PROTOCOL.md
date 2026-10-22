# Native RESP subset

| Command | Behavior |
|---|---|
| PING [message], ECHO message | Simple PONG or bulk echo |
| SET key value [NX] [PX milliseconds] | Positive TTL, NX optional; null on failed NX |
| GET key | Owned binary value, or null |
| DEL key, EXISTS key | One key only; integer result |
| INCR key | Signed integer, absent starts at zero, TTL retained |
| PTTL key | Milliseconds, -1 persistent, -2 missing/expired |
| SAVE | Atomic snapshot to configured server path |
| DBSIZE | Swept observational count |
| INFO [section] | Cinder counters; not complete Redis INFO compatibility |
| AUTH token | Connection-local authentication |
| QUIT | Close after OK |
| CONFIG GET name | Empty array, compatibility discovery only |
| CLIENT SETINFO name value, CLIENT SETNAME name | Accepted compatibility metadata, not retained |

All commands require RESP arrays of bulk strings; redis-cli uses this format. Command names/options are ASCII case insensitive. Maximum key length is 4096 bytes; value length 1 MiB, frame 2 MiB, arguments 16, native workers 64, queued native clients 64. Worker/queue limits are also applied independently to HTTP (4 workers). Socket I/O timeout is 2 seconds. Maximum configured payload is 64 MiB and each shard permits at most 32768 resident entries. Snapshot file cap is 256 MiB, entries 1048576. These limits deliberately make this a bounded demo. They are code configuration, not performance measurements.

Not supported: inline PING (redis-cli itself works), EX/XX/GET options, MGET/MSET, multi-key DEL, SELECT, MULTI/EXEC, Lua, replicas, pub/sub, Redis Cluster, RESP3 or arbitrary Redis tooling beyond the tested subset. redis-benchmark is run only for SET/GET. Do not point applications at this as a drop-in Redis replacement.
