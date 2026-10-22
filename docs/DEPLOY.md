# Deployment preparation — not deployed

Local use is `make demo`. Docker use is `docker compose up --build` with CINDER_TOKEN supplied through the environment. The image runs as a nonroot user. CI builds and actually starts the image and checks snapshot recovery after replacing the container. Docker is unavailable on the development Mac; container evidence must come from CI, not be inferred from native tests.

`render.yaml` is an unactivated free-plan Docker web-service Blueprint. Nothing has been provisioned. Owner approval is required before creating a service, connecting it or deploying.

After approval: create a Render Blueprint from this repository, review the Free plan and generated CINDER_TOKEN, deploy, and enter that token in the viewer over HTTPS. The Render frontend reaches HTTP on the configured port. Native RESP is not exposed by a public Render web-service endpoint; redis-cli and redis-benchmark comparisons remain local or inside a trusted container network. Health uses the public viewer root, not protected engine data. No backend data is exposed without its token.

The [Render free-service documentation](https://render.com/docs/free) states that its filesystem is ephemeral. The prepared free configuration therefore cannot provide durable snapshots across machine replacement/redeploy. Do not call SAVE durable on this host. A persistent disk and reviewed backup/restore policy are a separate paid hosting decision. The local Docker named volume and local native snapshot tests demonstrate durability only on their stated filesystems. Free-plan availability and limits are provider policy; verify them at deployment time.

No TLS in native RESP, ACLs, audit trail, tenant isolation, hosted capacity claim or production availability guarantee. Logs omit payloads and tokens. Use synthetic data only.
