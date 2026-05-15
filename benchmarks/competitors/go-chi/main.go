// Go + chi + pgx minimal /products endpoint. Run with:
//   go mod tidy && PORT=9004 go run ./...
package main

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"os"
	"strconv"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

var pool *pgxpool.Pool

type product struct {
	ID          int64   `json:"id"`
	Name        string  `json:"name"`
	Description string  `json:"description"`
	Price       float64 `json:"price"`
	Stock       int     `json:"stock"`
	Category    string  `json:"category"`
}

func listProducts(w http.ResponseWriter, r *http.Request) {
	limit := 20
	if s := r.URL.Query().Get("page_size"); s != "" {
		if n, err := strconv.Atoi(s); err == nil && n > 0 {
			limit = n
		}
	}
	ctx := r.Context()
	rows, err := pool.Query(ctx,
		"SELECT id, name, description, price, stock, category FROM products LIMIT $1",
		limit)
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	defer rows.Close()
	out := make([]product, 0, limit)
	for rows.Next() {
		var p product
		if err := rows.Scan(&p.ID, &p.Name, &p.Description, &p.Price, &p.Stock, &p.Category); err != nil {
			http.Error(w, err.Error(), 500)
			return
		}
		out = append(out, p)
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(out)
}

func main() {
	cfg, err := pgxpool.ParseConfig("host=/var/run/postgresql database=ecom pool_max_conns=32 pool_min_conns=8")
	if err != nil {
		log.Fatal(err)
	}
	pool, err = pgxpool.NewWithConfig(context.Background(), cfg)
	if err != nil {
		log.Fatal(err)
	}
	defer pool.Close()

	r := chi.NewRouter()
	r.Get("/products", listProducts)

	port := os.Getenv("PORT")
	if port == "" {
		port = "9004"
	}
	log.Printf("go-chi listening :%s", port)
	log.Fatal(http.ListenAndServe(":"+port, r))
}
