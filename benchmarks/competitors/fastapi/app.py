"""FastAPI + asyncpg minimal `/products` endpoint for benchmarking.

Run with:
    python3 -m uvicorn app:app --host 127.0.0.1 --port 9002 \\
        --workers 1 --log-level warning --app-dir <this-dir>
"""
import asyncpg
from fastapi import FastAPI

app = FastAPI()
pool: asyncpg.Pool | None = None


@app.on_event("startup")
async def _init():
    global pool
    pool = await asyncpg.create_pool(
        host="/var/run/postgresql",
        database="ecom",
        min_size=8,
        max_size=32,
    )


@app.on_event("shutdown")
async def _shutdown():
    if pool is not None:
        await pool.close()


@app.get("/products")
async def list_products(page_size: int = 20):
    async with pool.acquire() as conn:
        rows = await conn.fetch(
            "SELECT id, name, description, price, stock, category "
            "FROM products LIMIT $1",
            page_size,
        )
    return [dict(r) for r in rows]
