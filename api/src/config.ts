import dotenv from 'dotenv';
import path from 'path';


dotenv.config({
    path: path.resolve(__dirname, '../../.env')
});


function required(name: string): string {
    const value = process.env[name];

    if (!value) throw new Error(`Missing required environment variable: ${name}`);

    return value;
};


export const config = {
    databaseUrl: required('DATABASE_URL_POOLED'),
    redisUrl: required('REDIS_URL'),
    apiPort: Number(process.env.API_PORT ?? 3000),
    corsOrigin: process.env.CORS_ORIGIN ?? 'https://localhost:5173',
}