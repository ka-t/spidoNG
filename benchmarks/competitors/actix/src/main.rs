// Actix-web + tokio-postgres minimal `/products` endpoint.
// Run with: cargo build --release && PORT=9005 ./target/release/actix_bench
use std::env;

use actix_web::{web, App, HttpResponse, HttpServer, Responder};
use deadpool_postgres::{Config, ManagerConfig, Pool, RecyclingMethod, Runtime};
use serde::Serialize;
use tokio_postgres::NoTls;

#[derive(Serialize)]
struct Product {
    id: i64,
    name: String,
    description: Option<String>,
    price: f64,
    stock: i32,
    category: Option<String>,
}

#[derive(serde::Deserialize)]
struct Query {
    page_size: Option<i64>,
}

async fn list_products(
    pool: web::Data<Pool>,
    q: web::Query<Query>,
) -> impl Responder {
    let limit = q.page_size.unwrap_or(20);
    let client = match pool.get().await {
        Ok(c) => c,
        Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let stmt = match client
        .prepare_cached(
            "SELECT id, name, description, price, stock, category \
             FROM products LIMIT $1",
        )
        .await
    {
        Ok(s) => s,
        Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let rows = match client.query(&stmt, &[&limit]).await {
        Ok(r) => r,
        Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };

    let mut out: Vec<Product> = Vec::with_capacity(rows.len());
    for r in rows {
        out.push(Product {
            id:          r.get(0),
            name:        r.get(1),
            description: r.try_get(2).ok(),
            price:       r.get(3),
            stock:       r.get(4),
            category:    r.try_get(5).ok(),
        });
    }
    HttpResponse::Ok().json(out)
}

#[actix_web::main]
async fn main() -> std::io::Result<()> {
    let mut cfg = Config::new();
    cfg.host = Some("/var/run/postgresql".to_string());
    cfg.dbname = Some("ecom".to_string());
    cfg.user = Some(
        env::var("PGUSER").unwrap_or_else(|_| "kaan".to_string()),
    );
    cfg.manager = Some(ManagerConfig {
        recycling_method: RecyclingMethod::Fast,
    });

    let pool: Pool = cfg
        .create_pool(Some(Runtime::Tokio1), NoTls)
        .expect("pool");

    // Pre-warm pool: keep at least 8 connections hot. deadpool grows
    // lazily but we want to match the other stacks' min_size=8.
    let mut warmup = Vec::new();
    for _ in 0..8 {
        if let Ok(c) = pool.get().await { warmup.push(c); }
    }
    drop(warmup);

    let port = env::var("PORT")
        .ok()
        .and_then(|s| s.parse::<u16>().ok())
        .unwrap_or(9005);
    let workers: usize = env::var("WORKERS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(num_cpus_estimate());

    println!("actix listening :{} ({} workers)", port, workers);
    HttpServer::new(move || {
        App::new()
            .app_data(web::Data::new(pool.clone()))
            .route("/products", web::get().to(list_products))
    })
    .workers(workers)
    .bind(("127.0.0.1", port))?
    .run()
    .await
}

fn num_cpus_estimate() -> usize {
    std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(4)
}
