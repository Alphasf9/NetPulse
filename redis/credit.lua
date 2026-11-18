-- KEYS[1] = idemp:{gatewayTxnId}
-- KEYS[2] = user:{msisdn}:balance_bytes
-- KEYS[3] = user:{msisdn}:unlimited_5g
-- ARGV[1] = bytes to credit
-- ARGV[2] = "1" if the plan includes unlimited 5G, else "0"
-- ARGV[3] = marker lifetime in seconds

local idempKey       = KEYS[1]
local balanceKey     = KEYS[2]
local unlimitedKey   = KEYS[3]
local bytesToCredit  = tonumber(ARGV[1])
local planHas5g      = ARGV[2]
local markerLifetime = tonumber(ARGV[3])

-- 1. Claim the marker. Only the first caller succeeds.
local claimed = redis.call("SET", idempKey, "1", "NX", "EX", markerLifetime)
if not claimed then
  -- Duplicate delivery: change nothing, just report the current balance
  local current = tonumber(redis.call("GET", balanceKey) or "0")
  return {0, current}
end

-- 2. Credit the data
local new_balance = redis.call("INCRBY", balanceKey, bytesToCredit)

-- 3. Turn on 5G only if this plan includes it
if planHas5g == "1" then
  redis.call("SET", unlimitedKey, "1")
end

-- 4. {1 = credited just now, balance after}
return {1, new_balance}