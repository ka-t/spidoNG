// Actix multi-endpoint baseline for pressure isolation comparison.
// Three routes; no priority/admission/token-bucket. Every request hits
// the same pool with equal priority — the "naïve" model.
use std::env;

use actix_web::{web, App, HttpResponse, HttpServer, Responder};
use deadpool_postgres::{Config, ManagerConfig, Pool, RecyclingMethod, Runtime};
use serde::Serialize;
use tokio_postgres::NoTls;

#[derive(Serialize)]
struct Payment {
    id: i64, user_id: String, amount: f64, status: String,
}
#[derive(Serialize)]
struct Product {
    id: i64, name: String, description: Option<String>,
    price: f64, stock: i32, category: Option<String>,
}
#[derive(Serialize)]
struct AnalyticsEvent {
    id: i64, kind: String, payload: Option<String>,
}

async fn list_payments(pool: web::Data<Pool>) -> impl Responder {
    let client = match pool.get().await {
        Ok(c) => c,
        Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let stmt = match client.prepare_cached(
        "SELECT id, user_id, amount, status FROM payments LIMIT 20").await {
        Ok(s) => s, Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let rows = match client.query(&stmt, &[]).await {
        Ok(r) => r, Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let mut out: Vec<Payment> = Vec::with_capacity(rows.len());
    for r in rows {
        out.push(Payment {
            id: r.get(0), user_id: r.get(1), amount: r.get(2), status: r.get(3),
        });
    }
    HttpResponse::Ok().json(out)
}

async fn list_products(pool: web::Data<Pool>) -> impl Responder {
    let client = match pool.get().await {
        Ok(c) => c,
        Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let stmt = match client.prepare_cached(
        "SELECT id, name, description, price, stock, category FROM products LIMIT 20").await {
        Ok(s) => s, Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let rows = match client.query(&stmt, &[]).await {
        Ok(r) => r, Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let mut out: Vec<Product> = Vec::with_capacity(rows.len());
    for r in rows {
        out.push(Product {
            id: r.get(0), name: r.get(1), description: r.try_get(2).ok(),
            price: r.get(3), stock: r.get(4), category: r.try_get(5).ok(),
        });
    }
    HttpResponse::Ok().json(out)
}

async fn list_analytics(pool: web::Data<Pool>) -> impl Responder {
    let client = match pool.get().await {
        Ok(c) => c,
        Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let stmt = match client.prepare_cached(
        "SELECT id, kind, payload FROM analytics_events LIMIT 20").await {
        Ok(s) => s, Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let rows = match client.query(&stmt, &[]).await {
        Ok(r) => r, Err(e) => return HttpResponse::InternalServerError().body(e.to_string()),
    };
    let mut out: Vec<AnalyticsEvent> = Vec::with_capacity(rows.len());
    for r in rows {
        out.push(AnalyticsEvent {
            id: r.get(0), kind: r.get(1), payload: r.try_get(2).ok(),
        });
    }
    HttpResponse::Ok().json(out)
}

#[actix_web::main]
async fn main() -> std::io::Result<()> {
    let mut cfg = Config::new();
    cfg.host = Some("/var/run/postgresql".to_string());
    cfg.dbname = Some("ecom".to_string());
    cfg.user = Some(env::var("PGUSER").unwrap_or_else(|_| "kaan".to_string()));
    cfg.manager = Some(ManagerConfig { recycling_method: RecyclingMethod::Fast });
    let pool: Pool = cfg.create_pool(Some(Runtime::Tokio1), NoTls).expect("pool");

    let mut warmup = Vec::new();
    for _ in 0..8 { if let Ok(c) = pool.get().await { warmup.push(c); } }
    drop(warmup);

    let port: u16 = env::var("PORT").ok().and_then(|s| s.parse().ok()).unwrap_or(9101);
    let workers = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);

    println!("actix_multi listening :{} ({} workers)", port, workers);
    HttpServer::new(move || {
        App::new()
            .app_data(web::Data::new(pool.clone()))
            .route("/payments",  web::get().to(list_payments))
            .route("/products",  web::get().to(list_products))
            .route("/analytics", web::get().to(list_analytics))
    })
    .workers(workers)
    .bind(("127.0.0.1", port))?
    .run()
    .await
}
