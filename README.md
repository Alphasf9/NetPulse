::: {align="center"}
# ◈ NetPulse

### Real-Time Internet Usage · Data Quota Enforcement · Bandwidth Control

**See your internet. Measure every byte. Control your bandwidth.**

A telecom-inspired network metering and policy-enforcement platform
built to make the journey of a packet visible --- from a browser,
through a C++ proxy, into a real-time quota engine, and finally into an
auditable usage ledger.

`<br/>`{=html}

![Project
Status](https://img.shields.io/badge/status-in%20development-8b5cf6?style=for-the-badge)
![Node.js](https://img.shields.io/badge/Node.js-Control%20Plane-339933?style=for-the-badge&logo=nodedotjs&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-Traffic%20Plane-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)
![Redis](https://img.shields.io/badge/Redis-Live%20State-DC382D?style=for-the-badge&logo=redis&logoColor=white)
![PostgreSQL](https://img.shields.io/badge/PostgreSQL-Usage%20Ledger-4169E1?style=for-the-badge&logo=postgresql&logoColor=white)
![TypeScript](https://img.shields.io/badge/TypeScript-API-3178C6?style=for-the-badge&logo=typescript&logoColor=white)
:::

------------------------------------------------------------------------

> \[!IMPORTANT\] **NetPulse is a learning and engineering demonstration
> inspired by telecom data-plane and control-plane architecture.** It is
> not a real 4G/5G core network, mobile carrier, packet gateway, or
> cellular handover system. The 4G/5G selector represents an
> application-level policy mode. Real traffic is metered only when the
> client is configured to send that traffic through the NetPulse proxy.

## ✦ The idea

Imagine buying a prepaid data pack:

-   You receive a fixed data allowance.
-   Every byte that passes through the network gateway is measured.
-   Your remaining balance changes as you use the internet.
-   When the allowance is exhausted, the network applies a slower speed
    policy.
-   A special 5G policy can allow eligible traffic without deducting the
    4G quota.
-   Every usage event is eventually recorded for reporting and auditing.

NetPulse recreates the engineering ideas behind that experience in a
local, inspectable environment.

Instead of treating internet usage as a number typed into a dashboard,
NetPulse is designed around a real traffic path:

1.  A browser or test client sends a request to the local proxy.
2.  The C++ proxy identifies the subscriber and destination.
3.  The proxy asks Redis for an atomic quota reservation.
4.  The proxy forwards permitted traffic through the computer's normal
    network connection.
5.  The proxy counts transferred bytes and applies bandwidth policy.
6.  Usage events are published to Redis Streams.
7.  A Node.js worker persists usage records in PostgreSQL.
8.  The React dashboard presents subscriber balance, traffic activity,
    and policy state.

The central engineering challenge is coordinating **fast traffic
enforcement** with **accurate, durable accounting**.

## ✧ Product experience

The intended dashboard should make the system observable, not merely
attractive.

  -----------------------------------------------------------------------
  Experience                          What it should demonstrate
  ----------------------------------- -----------------------------------
  Subscriber overview                 Current plan, available quota, and
                                      policy state

  Live traffic meter                  Bytes transferred through the proxy

  Destination visibility              Hostname and port exposed by the
                                      proxy request

  Quota consumption                   Remaining allowance changing as
                                      metered traffic is used

  Network policy mode                 Simulated 4G / 5G policy selection

  Fair-use enforcement                A configured low-speed experience
                                      after quota exhaustion

  Event console                       Connection, reservation, transfer,
                                      and usage events

  Usage history                       Durable CDR-style records from
                                      PostgreSQL
  -----------------------------------------------------------------------

### Example prepaid policy

The original demonstration scenario uses a plan similar to this:

  -----------------------------------------------------------------------
  Policy                              Example behaviour
  ----------------------------------- -----------------------------------
  Plan price                          ₹19

  Metered allowance                   25 MB

  4G mode                             Deduct eligible bytes from the
                                      allowance

  5G mode                             If the subscriber is eligible, do
                                      not deduct the 4G allowance

  Exhausted allowance                 Apply a 64 kbps shaping policy
  -----------------------------------------------------------------------

These are **example policy values**, not a claim that a commercial
mobile operator's tariff or network behaviour is being reproduced
exactly. The plan values should ultimately live in the database rather
than being scattered through application code.

------------------------------------------------------------------------

# 1. System architecture

## 1.1 High-level architecture

``` mermaid
flowchart TB
    CLIENT["Browser / Test Client"]
    WEB["React Dashboard"]
    API["Node.js + Express API<br/>Control Plane"]
    PROXY["C++ TCP / HTTP CONNECT Proxy<br/>Traffic Plane"]
    REDIS[("Redis<br/>Live Policy + Quota + Streams")]
    WORKER["Node.js CDR Consumer"]
    DB[("PostgreSQL / Neon<br/>Durable Ledger")]
    SOCKET["Socket.IO"]

    CLIENT -->|"Configured proxy traffic"| PROXY
    WEB -->|"REST: plans, subscribers, recharge"| API
    WEB <-->|"Live dashboard events"| SOCKET
    API -->|"Read / update policy"| REDIS
    API -->|"Business records"| DB
    PROXY <-->|"Quota reservation / policy"| REDIS
    PROXY -->|"XADD usage event"| REDIS
    REDIS -->|"XREADGROUP"| WORKER
    WORKER -->|"Insert CDR"| DB
    API --> SOCKET
    WORKER -->|"Persisted usage / updates"| SOCKET
```

### The two most important planes

Telecom-inspired systems often separate policy and business decisions
from the high-throughput path that carries user traffic. NetPulse
follows the same conceptual separation.

  -----------------------------------------------------------------------
  Plane                   NetPulse component      Responsibility
  ----------------------- ----------------------- -----------------------
  Control plane           Node.js / Express       Subscribers, plans,
                                                  recharge, policy
                                                  configuration, APIs

  Traffic plane           C++ proxy               Accept connections,
                                                  forward traffic, meter
                                                  bytes, enforce limits
  -----------------------------------------------------------------------

**Why separate them?**

The API should not have to process every network byte. Its job is to
manage the system. The proxy is responsible for the hot path where
traffic is actually flowing.

For example, changing a subscriber's policy is a control-plane
operation. Applying that policy while transferring a response is a
traffic-plane operation.

## 1.2 Component responsibilities

``` mermaid
flowchart LR
    subgraph CONTROL["CONTROL PLANE"]
      A["Express Routes"]
      B["Subscriber / Plan Services"]
      C["Recharge & Policy Logic"]
    end

    subgraph FASTPATH["TRAFFIC PLANE"]
      D["TCP Listener"]
      E["Proxy Connection Handler"]
      F["Byte Meter"]
      G["Token Bucket"]
    end

    subgraph STATE["SHARED STATE"]
      H[("Redis")]
    end

    subgraph STORAGE["DURABLE STORAGE"]
      I[("PostgreSQL")]
      J["CDR Worker"]
    end

    A --> B --> C
    C <--> H
    D --> E --> F --> G
    E <--> H
    E --> H
    H --> J --> I
```

-   **React frontend:** gives a human-readable view of subscribers,
    quota, traffic and events.
-   **Express API:** exposes HTTP endpoints and coordinates business
    operations.
-   **C++ proxy:** sits in the configured client's traffic path and
    performs byte-level metering.
-   **Redis:** provides low-latency state shared by the API and proxy.
-   **Redis Lua scripts:** perform quota decisions and reservations
    atomically.
-   **Redis Streams:** carry usage events from the proxy to the
    accounting worker.
-   **CDR worker:** consumes usage events and stores them in PostgreSQL.
-   **PostgreSQL / Neon:** holds durable records used for history,
    billing and reporting.
-   **Socket.IO:** pushes selected live updates to connected dashboards.
-   **Docker Compose:** runs local infrastructure and, as the project
    matures, the local services.

## 1.3 Data ownership: Redis versus PostgreSQL

Redis and PostgreSQL are not competing databases in this architecture.
They have different jobs.

  -----------------------------------------------------------------------
  Data                                    Redis                PostgreSQL
  ------------------- ------------------------- -------------------------
  Current quota              Primary live value        Durable/accounting
  balance                                               representation as
                                                                 designed

  Current RAT /                             Yes May retain policy history
  policy mode                                   

  Short-lived quota                         Yes     Optional audit record
  reservations                                  

  Device proxy-token     Yes, preferably hashed         Subscriber/device
  lookup                                  token                 ownership

  Live rate-limit                           Yes                        No
  state                                         

  Usage event stream                 Yes, until       Durable CDR records
                         acknowledged/retention 
                                         policy 

  Subscribers and       Cache / live projection           Source of truth
  plans                               if useful 

  Recharge             Idempotency coordination           Source of truth
  transactions                        if needed 

  Historical usage                           No                       Yes
  reports                                       
  -----------------------------------------------------------------------

**A useful rule:** Redis answers, "What should happen to this connection
right now?" PostgreSQL answers, "What happened, and what must we be able
to prove later?"

Redis running in Docker is still a real Redis server. It stores data
inside its container/volume on the development machine. Redis Insight,
if installed, is only a graphical client for inspecting that same Redis
instance; it is not another database.

------------------------------------------------------------------------

# 2. How a request travels through NetPulse

## 2.1 The traffic lifecycle

``` mermaid
sequenceDiagram
    autonumber
    participant C as Browser / Client
    participant P as C++ Proxy
    participant R as Redis
    participant W as CDR Worker
    participant D as PostgreSQL
    participant UI as Dashboard

    C->>P: Connect to configured proxy
    P->>P: Authenticate subscriber
    P->>P: Parse destination / CONNECT
    P->>R: Request quota slice (Lua)
    R-->>P: Granted bytes + remaining balance
    alt Slice granted
        P->>C: Establish permitted tunnel / response
        P->>P: Forward and meter bytes
        P->>P: Apply bandwidth policy
        P->>R: XADD usage event
        R-->>P: Stream entry ID
        R->>W: Deliver stream entry
        W->>D: Insert CDR idempotently
        D-->>W: Commit
        W->>R: XACK stream entry
        W-->>UI: Publish persisted usage update
    else No quota granted
        P->>P: Apply exhausted-quota policy
    end
```

### Step-by-step explanation

**1. The client connects to the proxy**

The browser or test client must be explicitly configured to use the
proxy. The proxy is not automatically in the path of every application
on the laptop.

**2. The proxy identifies the subscriber**

For a controlled demo, use a device-specific, high-entropy proxy token.
Avoid using a phone number as a password. If HTTP Proxy Authentication
is used, remember that Basic authentication is Base64 encoding, not
encryption. Do not expose an unprotected proxy to the public internet.

**3. The proxy determines the destination**

For an HTTP request, the proxy can parse the request target. For HTTPS,
browsers normally use the `CONNECT host:port` method to request a TCP
tunnel.

**4. The proxy requests quota**

Before allowing a metered amount of traffic, the proxy asks Redis for a
bounded slice of quota. The quota script decides how many bytes can be
granted.

**5. Traffic is forwarded and counted**

The proxy copies bytes between the client socket and upstream socket. It
counts the bytes it actually transfers and applies the configured
shaping policy.

**6. Usage is emitted**

The proxy publishes a usage event. The event should contain enough
information to identify the subscriber, connection, reservation,
measured bytes and event time.

**7. Usage is persisted**

A Node.js consumer reads the event from a Redis Stream, writes it to
PostgreSQL and acknowledges it only after the database transaction
succeeds.

------------------------------------------------------------------------

# 3. Quota enforcement

## 3.1 Why quota reservation exists

Suppose a subscriber has 10 MB remaining and several browser connections
start at nearly the same time.

If every connection independently reads "10 MB remaining" and then
deducts its own usage, two connections could both believe they can spend
the same bytes.

That is a race condition.

A reservation model avoids letting every connection spend the same
balance.

``` mermaid
flowchart TD
    A["Connection needs more allowance"] --> B["Request a slice from Redis"]
    B --> C{"Atomic policy decision"}
    C -->|"Eligible unlimited policy"| D["Grant requested slice<br/>without metered deduction"]
    C -->|"Metered policy + balance > 0"| E["Grant min(request, balance)<br/>and deduct reservation"]
    C -->|"No balance"| F["Grant zero"]
    D --> G["Proxy transfers and counts bytes"]
    E --> G
    F --> H["Apply exhausted-quota policy"]
    G --> I["Settle reservation"]
    I --> J["Refund unused reserved bytes once"]
    J --> K["Publish usage event"]
```

A reservation is not the same as final usage:

-   **Granted:** bytes temporarily allocated to a connection.
-   **Consumed:** bytes actually transferred and counted.
-   **Unused:** granted bytes minus consumed bytes.
-   **Settled:** the reservation has been closed out, and any unused
    amount has been returned according to policy.

For a metered reservation:

`unused_bytes = granted_bytes - consumed_bytes`

The unused amount must be refunded exactly once. A connection must never
be able to refund the same reservation twice.

## 3.2 Atomic quota decisions with Redis Lua

Redis executes a Lua script atomically with respect to other commands on
that Redis server. No other client command can interleave in the middle
of the script.

That makes Lua useful for a read-check-update operation such as:

1.  Read the subscriber's policy.
2.  Read the available balance.
3.  Decide how much to grant.
4.  Deduct the grant.
5.  Return the result.

Illustrative policy logic:

``` lua
-- Illustrative logic only. Production code must validate arguments,
-- validate key types, and implement reservation IDs and settlement.

local requested = tonumber(ARGV[1])
local rat = redis.call("GET", KEYS[2])
local unlimited_5g = redis.call("GET", KEYS[3])
local balance = tonumber(redis.call("GET", KEYS[1]) or "0")

if rat == "5G_NR" and unlimited_5g == "1" then
    return {requested, balance}
end

if balance <= 0 then
    return {0, balance}
end

local granted = math.min(balance, requested)
local new_balance = redis.call("DECRBY", KEYS[1], granted)

return {granted, new_balance}
```

### What this example demonstrates

  -----------------------------------------------------------------------
  Condition                               Grant       Deduct from metered
                                                                 balance?
  ------------------- ------------------------- -------------------------
  Eligible unlimited           Requested amount                        No
  5G policy                                     

  Metered policy,      Smaller of requested and                       Yes
  balance available                     balance 

  Metered policy,                          Zero                        No
  balance exhausted                             
  -----------------------------------------------------------------------

The script is a teaching example of atomic policy evaluation. It is
**not yet a complete reservation accounting system**: a
production-quality implementation also needs reservation IDs, exact-once
settlement/refund, expiry and crash recovery.

## 3.3 Reservation state machine

``` mermaid
stateDiagram-v2
    [*] --> Requested
    Requested --> Granted: Redis grants bytes
    Requested --> Denied: No quota / policy denies
    Granted --> Active: Connection consumes bytes
    Active --> Settling: Connection closes or lease ends
    Settling --> Settled: Usage recorded and unused bytes handled
    Denied --> [*]
    Settled --> [*]
```

Important invariants:

-   A reservation belongs to one connection or explicitly defined owner.
-   A reservation has a unique ID.
-   Consumed bytes cannot exceed the granted amount.
-   Settlement is idempotent.
-   A refund is never applied twice.
-   A process crash must not permanently strand reserved quota.
-   The accounting policy must clearly define whether upload, download
    or both directions consume quota.

## 3.4 Aggregate bandwidth shaping

A subscriber's bandwidth policy should be shared across that
subscriber's active connections.

If the 64 kbps policy is applied independently to five connections, the
subscriber could receive roughly five times the intended aggregate
throughput. That is why the token bucket should be associated with
subscriber state, not created as an unrelated limiter for every socket.

A token bucket has:

-   **Capacity:** maximum accumulated burst allowance.
-   **Refill rate:** how quickly tokens become available.
-   **Available tokens:** how much data may pass immediately.

``` mermaid
flowchart LR
    T["Time passes"] --> R["Refill tokens at configured rate"]
    R --> B[("Shared subscriber token bucket")]
    B -->|"Enough tokens"| A["Allow bytes and consume tokens"]
    B -->|"Not enough tokens"| W["Wait for refill"]
    W --> B
    A --> C["Continue forwarding"]
```

For a 64 kilobit-per-second limit:

-   64 kilobits = 8 kilobytes per second (before protocol overhead).
-   The implementation should use a monotonic clock such as
    `std::chrono::steady_clock`.
-   A mutex or another suitable synchronization strategy is needed if
    several connection handlers share the same bucket.
-   Upload and download shaping should be specified separately if the
    product requires different rates.

------------------------------------------------------------------------

# 4. Usage accounting: Redis Streams and CDRs

## 4.1 Why not just write directly to PostgreSQL from C++?

The proxy is responsible for the live traffic path. Making every
connection wait on a database write would couple traffic handling to
database latency and availability.

Instead, the proxy can publish compact usage events to Redis Streams. A
Node.js worker handles durable persistence separately.

``` mermaid
flowchart LR
    P["C++ Proxy"] -->|"XADD"| S[("Redis Stream")]
    S -->|"XREADGROUP"| W["Node.js Consumer"]
    W -->|"BEGIN / INSERT"| DB[("PostgreSQL")]
    DB -->|"COMMIT"| W
    W -->|"XACK after commit"| S
```

## 4.2 Why Redis Streams?

Redis Streams provide a persistent, ordered event log within Redis,
consumer groups, pending-entry tracking and acknowledgement.

This is useful because a worker can fail after receiving a message. An
unacknowledged entry remains pending and can be recovered by another
consumer according to the recovery strategy.

The intended processing order is:

1.  Read an event from the consumer group.
2.  Validate its fields.
3.  Insert the CDR using a unique event identifier.
4.  Commit the database transaction.
5.  Acknowledge the Redis Stream entry.

**Do not acknowledge before the database commit.**

## 4.3 Delivery guarantees and idempotency

Redis Streams consumer groups provide an at-least-once processing
pattern. A message may be delivered again if a worker crashes or loses
its acknowledgement.

That is expected. The database write must be idempotent.

A recommended pattern:

-   Give each usage event a stable unique `event_id`.
-   Add a unique constraint on that ID in PostgreSQL.
-   Insert with conflict handling.
-   Acknowledge the stream entry after the transaction succeeds.

This does not magically create exactly-once delivery. It creates **safe,
repeatable processing** despite possible redelivery.

## 4.4 Example CDR fields

A CDR (Call Detail Record) in this project is a usage/accounting record,
not necessarily a literal carrier-grade telecom CDR.

  Field                Purpose
  -------------------- -----------------------------------------------
  `event_id`           Stable unique ID for idempotency
  `subscriber_id`      Subscriber associated with the traffic
  `connection_id`      Proxy connection identifier
  `reservation_id`     Quota reservation identifier, when applicable
  `destination_host`   Hostname visible to the proxy
  `started_at`         Connection start time
  `ended_at`           Connection end time
  `upload_bytes`       Bytes transferred client-to-upstream
  `download_bytes`     Bytes transferred upstream-to-client
  `billable_bytes`     Bytes charged under the active policy
  `rat_type`           Policy mode at the relevant time
  `policy_version`     Version or identifier of the policy applied

The exact schema should be finalized with the database migrations. The
table above describes the intended information, not a guarantee that
every field is already implemented.

------------------------------------------------------------------------

# 5. Technology stack

  Technology                 Role in NetPulse
  -------------------------- ----------------------------------------------------------
  React                      Dashboard UI
  TypeScript                 Frontend and backend application code
  Node.js                    API and background worker runtime
  Express                    HTTP API routing
  C++                        TCP proxy and traffic metering
  Redis                      Live quota, policy state, reservations and event streams
  Redis Lua                  Atomic quota policy operations
  Redis Streams              Proxy-to-worker usage event transport
  PostgreSQL / Neon          Durable business and usage data
  Drizzle ORM                Type-safe database schema and queries
  Socket.IO                  Live dashboard updates
  Docker / Docker Compose    Reproducible local services
  Redis Insight (optional)   GUI for inspecting Redis data

## 5.1 Why C++ for the proxy?

The proxy handles sockets and moves bytes. C++ offers direct access to
operating-system networking APIs and explicit control over memory,
buffers, threads and I/O.

The current development direction uses POSIX sockets in a Linux
environment. Since the development machine may be Windows, Docker can
provide a Linux container for the proxy.

This is not because Node.js cannot create a proxy. It is an intentional
learning choice: the project explores low-level socket programming and
traffic handling separately from the business API.

## 5.2 Why Redis for live quota?

A quota decision may happen repeatedly while a connection is active.
Redis provides low-latency shared state, and Lua lets the project make a
quota decision and update the balance as one atomic operation.

PostgreSQL remains responsible for durable records. Redis should not
become the only record of a financial or usage history that must survive
operational failures.

## 5.3 Why Drizzle ORM?

Drizzle provides a TypeScript-oriented way to define PostgreSQL tables
and query them.

For example, a foreign-key declaration such as:

``` typescript
userId: integer("user_id")
  .notNull()
  .references(() => users.id)
```

means `recharge_transactions.user_id` references `users.id`. The
callback tells Drizzle which column is the reference target; the
schema/migration process creates the actual database constraint.

------------------------------------------------------------------------

# 6. Repository structure

The current repository layout shared during development is:

``` text
NetPulse/
│
├── api/
│   ├── src/
│   │   ├── db/
│   │   ├── realtime/
│   │   ├── routes/
│   │   ├── services/
│   │   ├── workers/
│   │   ├── config.ts
│   │   └── server.ts
│   ├── drizzle.config.ts
│   ├── package.json
│   └── package-lock.json
│
├── proxy/
│   └── ... C++ proxy source and build files ...
│
├── redis/
│   └── ... Redis scripts and configuration ...
│
├── web/
│   └── ... React frontend ...
│
├── .env
├── .env.example
├── .gitignore
└── docker-compose.yml
```

The structure is intentionally separated by responsibility:

-   `api/src/routes/` --- HTTP endpoints.
-   `api/src/services/` --- business and policy operations called by
    routes.
-   `api/src/db/` --- Drizzle schema, database client and
    migrations-related code.
-   `api/src/realtime/` --- Socket.IO setup and event delivery.
-   `api/src/workers/` --- background consumers and asynchronous
    processing.
-   `api/src/config.ts` --- central environment configuration.
-   `api/src/server.ts` --- API process entry point.
-   `proxy/` --- C++ traffic-plane implementation.
-   `redis/` --- Lua scripts and Redis-related configuration.
-   `web/` --- React dashboard.

> \[!NOTE\] This tree reflects the structure shown during development.
> Some directories may still be empty or under construction. Add files
> to the diagram as the implementation grows rather than treating this
> as a claim that every subsystem is complete.

------------------------------------------------------------------------

# 7. Local development

## 7.1 Prerequisites

Install or prepare:

-   Git
-   Node.js (use a current supported LTS release)
-   npm
-   Docker Desktop with its Linux container engine enabled
-   A Neon PostgreSQL database, or another PostgreSQL instance for local
    development
-   A C++ compiler and build tooling inside the Linux proxy environment
-   A modern browser for testing the dashboard and proxy

For Windows development, PowerShell commands below assume the repository
is at `E:\NetPulse`. Paths may be different on another machine.

## 7.2 Environment variables

The API configuration currently expects values such as:

``` dotenv
DATABASE_URL_POOLED=your_neon_pooled_connection_string
REDIS_URL=redis://localhost:6379
API_PORT=3000
CORS_ORIGIN=http://localhost:5173
```

The actual `.env.example` in the repository is the source of truth for
the complete list of required variables.

**Never commit `.env` or real credentials.** Keep placeholders in
`.env.example`, and keep secrets only in the local environment or a
proper secret manager.

If the API is running inside Docker Compose, `localhost` means the API
container itself. In that case, the Redis hostname is normally the
Compose service name, such as `redis`, rather than `localhost`. Use the
connection URL appropriate to where the API process runs.

## 7.3 Start infrastructure

From the repository root:

``` powershell
cd E:\NetPulse
docker compose up -d
docker compose ps
```

To inspect service logs:

``` powershell
docker compose logs -f
```

To stop the services:

``` powershell
docker compose down
```

To stop services and remove their named volumes (this can delete local
database data):

``` powershell
docker compose down -v
```

Use the last command only when you intentionally want to reset local
persisted data.

## 7.4 Start the API

The API package uses TypeScript and already includes `tsx` as a
development dependency.

From the API directory:

``` powershell
cd E:\NetPulse\api
npm install
npm run dev
```

The development script should watch the TypeScript entry point:

``` json
{
  "scripts": {
    "dev": "tsx watch src/server.ts",
    "start": "tsx src/server.ts"
  }
}
```

`tsx watch` restarts the process when watched source files change. It
runs TypeScript directly; it does not generate a production `dist/`
build.

## 7.5 Start the frontend

The frontend is located in `web/`. Check its `package.json` for the
scripts actually provided by the frontend setup. A typical Vite workflow
is:

``` powershell
cd E:\NetPulse\web
npm install
npm run dev
```

Do not assume the frontend's port or script name without checking its
package file. Vite commonly uses port `5173`, but the actual output in
the terminal is authoritative.

## 7.6 Proxy development

The C++ proxy is intended to run in a Linux environment because its
socket implementation uses POSIX APIs.

The exact build and run commands depend on the current `proxy/` source
and Dockerfile. Once those are in place, document the compiler, build
target, listen port, Redis connection and test-client configuration
here.

------------------------------------------------------------------------

# 8. Testing real traffic

NetPulse is designed to meter real internet traffic that passes through
its proxy. It does not need to fabricate a download counter to
demonstrate byte movement.

A controlled test should look like this:

``` mermaid
flowchart TD
    A["Start Redis and API"] --> B["Start C++ proxy"]
    B --> C["Configure a test browser to use proxy"]
    C --> D["Open a known HTTP / HTTPS destination"]
    D --> E["Proxy records destination and bytes"]
    E --> F["Quota policy is applied"]
    F --> G["Usage event reaches Redis Stream"]
    G --> H["CDR worker persists event"]
    H --> I["Dashboard displays updated usage"]
```

### Important networking boundaries

**The proxy only meters traffic that traverses it.** Configuring one
browser to use the proxy does not automatically route every application
on the computer through it.

**HTTPS visibility is limited.** With an HTTPS `CONNECT` tunnel, the
proxy can generally see the requested destination hostname and port and
count the bytes crossing the tunnel. It cannot read encrypted page
paths, message contents, passwords, video titles or other HTTPS payload
data.

**HTTP CONNECT is a TCP tunnel.** It does not automatically proxy
arbitrary UDP traffic. Browsers may use HTTP/3 over QUIC/UDP, which can
bypass a basic HTTP CONNECT proxy or require a fallback to TCP. For an
initial controlled test, use a client that honours the proxy and ensure
the test traffic uses the supported TCP path.

**This is not a transparent network gateway.** Capturing all device
traffic requires additional routing, VPN, transparent proxy or gateway
configuration. That is outside the first controlled-browser milestone.

------------------------------------------------------------------------

# 9. Security and correctness principles

These are engineering requirements, not optional polish.

## 9.1 Authentication

-   Use high-entropy device-specific proxy credentials.
-   Store token hashes where practical; do not log raw tokens.
-   Do not treat a phone number as a secret.
-   Basic Proxy Authentication encodes credentials but does not encrypt
    them.
-   Do not expose an unauthenticated proxy to an untrusted network.
-   Restrict development services to localhost unless remote access is
    explicitly required.

## 9.2 Quota integrity

-   All quota grants and deductions must go through the atomic policy
    path.
-   Reservations must have unique IDs.
-   Settlement and refund operations must be idempotent.
-   Never refund an entire reservation without checking actual
    consumption.
-   Handle connection close, timeout and process-crash recovery.
-   Define exactly which byte directions are billable.
-   Test concurrent connections against one subscriber balance.

## 9.3 Recharge integrity

A payment callback may be delivered more than once. A safe recharge flow
should:

1.  Verify the payment provider's signature.
2.  Identify the transaction using a unique provider transaction ID.
3.  Use a database transaction and uniqueness constraints to prevent
    duplicate credit.
4.  Record the successful recharge durably.
5.  Update the live Redis projection in a recoverable way.
6.  Reconcile or retry if PostgreSQL and Redis temporarily disagree.

For a learning build, a mock payment provider is sufficient. Do not
connect real payments until the idempotency and reconciliation behaviour
is understood and tested.

## 9.4 Secrets and operational safety

-   Keep `.env` out of Git.
-   Use `.env.example` for variable names and harmless placeholders.
-   Avoid logging authorization headers or payment secrets.
-   Bind development-only ports to localhost where possible.
-   Use TLS and appropriate authentication before exposing services
    beyond the local machine.
-   Define retention and access rules for subscriber usage records.

------------------------------------------------------------------------

# 10. Observability

A system that meters traffic should make its own decisions inspectable.

Useful structured events include:

  -----------------------------------------------------------------------
  Event                               Example fields
  ----------------------------------- -----------------------------------
  `proxy.connection.accepted`         connection ID, client address,
                                      timestamp

  `proxy.auth.succeeded`              subscriber ID, connection ID

  `proxy.auth.failed`                 reason category, timestamp

  `quota.reservation.granted`         reservation ID, requested, granted,
                                      balance

  `quota.reservation.denied`          subscriber ID, requested, balance

  `proxy.bytes.transferred`           upload bytes, download bytes

  `bandwidth.policy.applied`          subscriber ID, configured rate

  `cdr.published`                     event ID, stream ID

  `cdr.persisted`                     event ID, database record ID

  `cdr.duplicate`                     event ID
  -----------------------------------------------------------------------

Avoid logging secrets, full authorization headers or encrypted payload
contents.

### Suggested dashboard panels

-   Active proxy connections
-   Total upload and download throughput
-   Subscriber quota remaining
-   Current policy mode
-   Current bandwidth cap
-   Recent destinations (hostnames only, where available)
-   Latest quota decisions
-   Recent CDR persistence status
-   Worker pending-message count and consumer health

------------------------------------------------------------------------

# 11. Development milestones

The following roadmap describes the intended build sequence. It is a
plan, not a statement that every item is already implemented.

``` mermaid
flowchart TD
    M1["01 · Foundation<br/>Repository, config, Docker, Redis, DB"]
    M2["02 · Control Plane<br/>API, schema, plans, subscribers"]
    M3["03 · Quota Engine<br/>Redis keys and Lua policy"]
    M4["04 · C++ Proxy<br/>Sockets, CONNECT, authentication"]
    M5["05 · Traffic Metering<br/>Byte counters and shaping"]
    M6["06 · Accounting Pipeline<br/>Redis Streams and CDR worker"]
    M7["07 · Live Dashboard<br/>React and Socket.IO"]
    M8["08 · Hardening<br/>Idempotency, recovery, tests, security"]

    M1 --> M2 --> M3 --> M4 --> M5 --> M6 --> M7 --> M8
```

  -----------------------------------------------------------------------
  Milestone               Outcome                 Acceptance check
  ----------------------- ----------------------- -----------------------
  01\. Foundation         Local services and      API can connect to
                          configuration           Redis and PostgreSQL

  02\. Control plane      Subscriber and plan     API returns persisted
                          APIs                    plan/subscriber data

  03\. Quota engine       Atomic quota decisions  Concurrent tests cannot
                                                  overspend the same
                                                  balance

  04\. C++ proxy          TCP listener and        A configured test
                          supported proxy flow    client can reach an
                                                  upstream destination

  05\. Traffic metering   Byte counts and shaping Transferred bytes are
                                                  measured and the
                                                  configured cap is
                                                  observable

  06\. CDR pipeline       Stream consumer and     A published event is
                          durable records         persisted and safely
                                                  acknowledged

  07\. Live dashboard     Live state and usage    A real proxy test
                          view                    changes the dashboard

  08\. Hardening          Failure handling and    Duplicate events,
                          security                crashes and concurrent
                                                  use are tested
  -----------------------------------------------------------------------

------------------------------------------------------------------------

# 12. Engineering decisions and trade-offs

  -----------------------------------------------------------------------
  Decision                Reason                  Trade-off / limitation
  ----------------------- ----------------------- -----------------------
  C++ proxy               Low-level socket and    More manual memory,
                          byte-flow learning      concurrency and error
                                                  handling

  Node.js control plane   Product APIs and        Not responsible for
                          business workflows      every data-plane byte

  Redis Lua               Atomic quota decision   Scripts need careful
                                                  validation and
                                                  versioning

  Redis Streams           Decoupled usage         At-least-once delivery
                          pipeline                requires idempotent
                                                  consumers

  PostgreSQL ledger       Durable, queryable      Slower than Redis for
                          history                 hot-path policy checks

  Shared subscriber token Aggregate speed cap     Shared state requires
  bucket                  across connections      synchronization

  HTTP CONNECT            Practical HTTPS tunnel  Does not inspect HTTPS
                          for a demo              content or cover
                                                  arbitrary UDP

  Docker-based Linux      POSIX networking APIs   Container networking
  proxy                   on a Windows            and port mapping must
                          workstation             be configured correctly
  -----------------------------------------------------------------------

## 12.1 Failure cases worth testing

-   Redis is unavailable when a new reservation is requested.
-   The proxy loses its Redis connection during an active transfer.
-   A connection closes before its reservation is fully consumed.
-   The proxy process crashes while reservations are outstanding.
-   A CDR event is delivered twice.
-   The worker crashes after database commit but before `XACK`.
-   PostgreSQL is unavailable while Redis continues receiving events.
-   Two connections request the last remaining quota simultaneously.
-   A subscriber changes policy while a connection is active.
-   A browser attempts a UDP/QUIC path not supported by the proxy.

For each case, define whether the system fails open or closed. For quota
enforcement, a conservative design generally avoids granting unmetered
traffic when policy state cannot be verified, but the exact behaviour
must be an explicit product decision.

------------------------------------------------------------------------

# 13. Glossary

  -----------------------------------------------------------------------
  Term                                Meaning in this project
  ----------------------------------- -----------------------------------
  Subscriber                          The account whose traffic and quota
                                      are managed

  Proxy                               An intermediary that receives a
                                      client's request and forwards it
                                      upstream

  CONNECT                             HTTP method commonly used to
                                      establish an HTTPS tunnel through a
                                      proxy

  Control plane                       APIs and services that configure
                                      and manage policy

  Traffic plane                       The component that handles live
                                      network traffic

  Quota                               The amount of data a subscriber is
                                      allowed to consume

  Reservation                         A temporary allocation of quota to
                                      a connection

  Settlement                          Final accounting of a reservation
                                      and any unused amount

  Token bucket                        A rate-limiting algorithm that
                                      controls how quickly data may pass

  RAT                                 Radio Access Technology;
                                      represented here as a simulated
                                      policy mode

  CDR                                 A usage record describing a
                                      connection or metered activity

  Lua script                          A server-side Redis script used for
                                      atomic multi-command operations

  Redis Stream                        An append-only event stream with
                                      consumer-group support

  Consumer group                      A Redis Streams mechanism for
                                      distributing event processing

  Idempotency                         The property that repeating an
                                      operation does not apply its effect
                                      twice

  Backpressure                        Slowing or pausing
                                      producers/consumers when downstream
                                      capacity is limited

  QUIC                                A UDP-based transport used by
                                      HTTP/3

  Neon                                A hosted serverless PostgreSQL
                                      platform
  -----------------------------------------------------------------------

------------------------------------------------------------------------

# 14. Project principles

NetPulse is being built around a few principles:

1.  **Real traffic over fake counters.** Demonstrations should be backed
    by traffic that actually traverses the proxy.
2.  **Correctness before cleverness.** Quota accounting must survive
    concurrency, retries and failure.
3.  **Fast path stays focused.** The proxy handles traffic; the API
    handles business operations.
4.  **Every important byte has an explanation.** Usage should be
    traceable from connection to reservation to CDR.
5.  **Live state and durable history are different concerns.** Redis and
    PostgreSQL each have a clear responsibility.
6.  **Security boundaries must be honest.** A proxy tunnel is not TLS
    decryption, and a simulated 5G mode is not a cellular network.
7.  **Build in verifiable milestones.** Each layer should have a test
    proving it works before the next layer depends on it.

------------------------------------------------------------------------

::: {align="center"}
### ◈ NetPulse

**From packets to policy. From usage to insight.**

*A hands-on exploration of network programming, distributed state,
real-time systems and reliable accounting.*
:::
