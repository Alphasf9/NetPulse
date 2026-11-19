#include <hiredis/hiredis.h>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

int main()
{
    // 1. Connect to Redis running in Docker, published on localhost:6379
    redisContext *ctx = redisConnect("127.0.0.1", 6379);
    if (ctx == nullptr || ctx->err)
    {
        std::cerr << "Connection error: " << (ctx ? ctx->errstr : "allocation failed") << "\n";
        return 1;
    }

    // 2. Send a command the way you did with redis-cli
    redisReply *reply = (redisReply *)redisCommand(ctx, "PING");
    std::cout << "PING reply type=" << reply->type << " str=" << (reply->str ? reply->str : "(null)") << "\n";
    freeReplyObject(reply);

    // 3. Try a real key, same idea as your earlier redis-cli GET tests
    reply = (redisReply *)redisCommand(ctx, "GET user:9999999999:balance_bytes");
    if (reply->type == REDIS_REPLY_NIL)
        std::cout << "key not found\n";
    else
        std::cout << "balance_bytes = " << reply->str << "\n";
    freeReplyObject(reply);

    redisFree(ctx);
    return 0;
}