import { pool } from './client';

async function main() {
    const result = await pool.query('SELECT NOW() as server_time');
    console.log('Database server time:', result.rows[0]);
    await pool.end();
}

main();