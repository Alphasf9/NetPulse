-- KEYS[1] = user:{msisdn}:balance_bytes
-- KEYS[2] = user:{msisdn}:rat_type
-- KEYS[3] = user:{msisdn}:unlimited_5g
-- ARGV[1] = requested slice size in bytes



local requested = tonumber(ARGV[1])
local rat= redis.call("GET", KEYS[2])
local unlimited_5g= redis.call("GET", KEYS[3])
local balance = tonumber(redis.call("GET", KEYS[1]) or "0")


-- 5G zero-rating: grant the slice, deduct nothing

if rat=="5G_NR" and unlimited_5g=="1"then
    return {requested, balance}

end


-- 4G with no data left: grant nothing (proxy will throttle)


if balance<=0 then
    return {0, balance}
end


-- Normal case: give min(what's left, what was asked for)


local granted = math.min(balance, requested)
local new_balance = redis.call("DECRBY", KEYS[1], granted)
return {granted, new_balance}